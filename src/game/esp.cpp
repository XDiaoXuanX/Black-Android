// The cheats page's ESP: each living enemy can show a line from the bottom of the screen to its feet and/or a
// health bar above its head. What shows is picked by esp_display's two bits (bit 0 = line, bit 1 = health bar),
// so 4 combinations from 0 (nothing) to 3 (line + health) are available from a single selector. The line and
// the health bar's background use the colour picked by esp_color (preset or esp_red / esp_green / esp_blue);
// the health bar's fill uses its own green/yellow/red palette (green above 66% of the reference max, yellow
// above 33%, red below). AIMBOT: the view turns to the point on the living enemy nearest the crosshair chosen
// by aimbot_target (neck, body, legs or head), by aimbot_pull each frame, but never inside aimbot_deadzone.
//
// The CROSSHAIR (options.h) is drawn in the same 2D layer, in the centre of the viewport, with its own shape
// (+ / * / X / DOT) and colour scheme (preset or custom RGB, like ESP). It is drawn first, before the ESP
// guard, so it appears even when ESP is off.
#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>

#include "common/options.h"
#include "game/layout.h"
#include "xdk/hle.h"
#include "xdk/layout.h"

using namespace kernel;
using namespace layout;

namespace {

// character: kind 1 an enemy (the game's aim assist takes only those), its position the one its update makes a
// velocity of. The world is Y up: that update zeroes Y for the speed along the ground
constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharHealth = 0x2F0;   // float, the character's current health
constexpr uint32_t kCharPosition = 0xA0;
constexpr uint32_t kCharDying = 0x384;
constexpr uint32_t kEnemyKind = 1;

// the player's controller: yaw positive left, pitch positive down, in degrees (mouse.cpp)
constexpr uint32_t kControllerYaw = 0x18;
constexpr uint32_t kControllerPitch = 0x1C;

constexpr float kHeight = 1.8f;      // world units are metres: the near plane is 0.08
constexpr float kBoxWidth = 0.45f;   // of the box's height — used for the health bar's width
constexpr float kNear = 0.08f;       // clip w nearer than this is behind the camera
constexpr float kThickness = 1.5f;   // lines of a 480 frame

// aimbot target heights above the feet, chosen by aimbot_target:
//   0 NECK: just under the chin. Between the neck hitbox (~1.50) and the jaw (~1.60): lethal like the head but
//     forgiving to the bob of a walking or crouching enemy that would make a 1.65 point float above the skull.
//   1 BODY: the chest centre. Centre mass, less lethal than the neck but much easier to land while the target
//     moves — the safe default for the player who wants hits, not just assists.
//   2 LEGS: the thighs. Slows a sprinting enemy without the two-shot kill, at the cost of the head being far
//     from the crosshair.
//   3 HEAD: the headshot line, between the jaw (~1.60) and the top of the skull (~1.75). Most lethal, but the
//     most sensitive to the bob of a walking or crouching enemy.
constexpr float kAimHeights[4] = {
    1.55f,  // NECK (default)
    1.15f,  // BODY
    0.45f,  // LEGS
    1.68f,  // HEAD
};

// the ESP line colours the preset row offers; panel cheats.cpp holds the same list with names (kEspPresets)
// and must be kept in sync with these — a preset picked there writes its rgb into esp_red / esp_green /
// esp_blue, and the CUSTOM entry reads them straight. The CROSSHAIR uses the same list via crosshair_color.
struct EspPreset {
    float r, g, b;
};
constexpr EspPreset kEspPresets[] = {
    {0.88f, 0.86f, 0.82f},  // WHITE (the soft white the ESP shipped with)
    {1.00f, 0.20f, 0.20f},  // RED
    {0.20f, 1.00f, 0.30f},  // GREEN
    {0.20f, 0.90f, 1.00f},  // CYAN
    {1.00f, 0.85f, 0.20f},  // YELLOW
    {1.00f, 0.30f, 1.00f},  // MAGENTA
    {1.00f, 0.55f, 0.10f},  // ORANGE
    {0.70f, 0.40f, 1.00f},  // PURPLE
};
constexpr size_t kEspPresetCount = sizeof(kEspPresets) / sizeof(kEspPresets[0]);

constexpr float kCone = 30.0f;       // degrees from the crosshair an enemy is taken in
constexpr float kDegrees = 180.0f / 3.14159265f;

// an enemy is drawn while its update ran this recently: none once the game pauses, no flicker between ticks
constexpr auto kSeen = std::chrono::milliseconds(250);
constexpr size_t kMaxEnemies = 128;

// Health bar reference: an enemy with this much health or more shows a full bar.
constexpr float kEnemyRefHealth = 250.0f;

// Health bar colours at the three thresholds (green >66%, yellow 33-66%, red <33%)
constexpr float kHealthColourHigh[4] = {0.20f, 0.90f, 0.20f, 1.0f};
constexpr float kHealthColourMid[4] = {1.00f, 0.85f, 0.10f, 1.0f};
constexpr float kHealthColourLow[4] = {1.00f, 0.20f, 0.20f, 1.0f};

struct Seen {
    uint32_t who;
    std::chrono::steady_clock::time_point at;
};
Seen g_seen[kMaxEnemies];
size_t g_count;

// the scene's view and projection, row vectors: clip = position * view * projection
float g_view[16], g_projection[16];
bool g_camera;
uint32_t g_camera_frame;  // counts the scene's draws: the aimbot turns once for each view it reads

// the 2D layer's untextured triangles (eax triangles, ecx their points; float x, y of the points' offset, then a
// float rgba of each point, 16 aligned): in its batch, drawn with its shader and states at the next flush
constexpr uint32_t kTriangles = 0xD5A00;
struct Vertex {
    float x, y;
};

// Per-enemy vertex budget:
//   line        : 6  (2 triangles)
//   health bar  : 12 (background + fill, 2 triangles each)
//   total max   : 18 → rounded to 24
constexpr size_t kVertsPerEnemy = 24;
constexpr size_t kMaxVertices = kMaxEnemies * kVertsPerEnemy;

// Crosshair vertex budget: the * shape is 8 arms × 6 verts = 48, round up to 64.
constexpr size_t kCrosshairMaxVerts = 64;

void Remember(uint32_t who) {
    auto now = std::chrono::steady_clock::now();
    for (size_t i = 0; i < g_count; i++)
        if (g_seen[i].who == who) {
            g_seen[i].at = now;
            return;
        }
    if (g_count < kMaxEnemies) {
        g_seen[g_count++] = {who, now};
        return;
    }
    Seen* oldest = g_seen;
    for (Seen& s : g_seen)
        if (s.at < oldest->at) oldest = &s;
    *oldest = {who, now};
}

// fills `out` with the rgba the ESP line and health bar background should use: a preset, or the custom rgb
// divided by 255
void CurrentColour(float out[4]) {
    const game::Options& opts = game::Opt();
    uint32_t preset = opts.esp_color;
    if (preset < kEspPresetCount) {
        out[0] = kEspPresets[preset].r;
        out[1] = kEspPresets[preset].g;
        out[2] = kEspPresets[preset].b;
    } else {
        out[0] = float(opts.esp_red) / 255.0f;
        out[1] = float(opts.esp_green) / 255.0f;
        out[2] = float(opts.esp_blue) / 255.0f;
    }
    out[3] = 1.0f;
}

// crosshair colour: same scheme, crosshair_color / crosshair_red / crosshair_green / crosshair_blue
void CurrentCrosshairColour(float out[4]) {
    const game::Options& opts = game::Opt();
    uint32_t preset = opts.crosshair_color;
    if (preset < kEspPresetCount) {
        out[0] = kEspPresets[preset].r;
        out[1] = kEspPresets[preset].g;
        out[2] = kEspPresets[preset].b;
    } else {
        out[0] = float(opts.crosshair_red) / 255.0f;
        out[1] = float(opts.crosshair_green) / 255.0f;
        out[2] = float(opts.crosshair_blue) / 255.0f;
    }
    out[3] = 1.0f;
}

// clip space; false behind the camera
bool Clip(const float p[3], float clip[4]) {
    float eye[4];
    for (int c = 0; c < 4; c++)
        eye[c] = p[0] * g_view[c] + p[1] * g_view[4 + c] + p[2] * g_view[8 + c] + g_view[12 + c];
    for (int c = 0; c < 4; c++)
        clip[c] = eye[0] * g_projection[c] + eye[1] * g_projection[4 + c] + eye[2] * g_projection[8 + c] +
                  eye[3] * g_projection[12 + c];
    return clip[3] > kNear;
}

// screen position in viewport pixels; false behind the camera
bool Project(const float p[3], float vx, float vy, float vw, float vh, float out[2]) {
    float clip[4];
    if (!Clip(p, clip)) return false;
    out[0] = vx + (clip[0] / clip[3] + 1) * 0.5f * vw;
    out[1] = vy + (1 - clip[1] / clip[3]) * 0.5f * vh;
    return true;
}

// Writes one segment (6 vertices, 2 triangles) to the vertex buffer at *v, and its 6 colours at *c.
void WriteSegment(Vertex*& v, uint8_t*& c, float x0, float y0, float x1, float y1, float width,
                  const float colour[4]) {
    float dx = x1 - x0, dy = y1 - y0;
    float length = std::sqrt(dx * dx + dy * dy);
    if (!(length > 0)) return;
    float nx = -dy / length * width * 0.5f, ny = dx / length * width * 0.5f;
    const float corners[4][2] = {{x0 + nx, y0 + ny}, {x0 - nx, y0 - ny}, {x1 + nx, y1 + ny}, {x1 - nx, y1 - ny}};
    for (int i : {0, 1, 2, 2, 1, 3}) {
        *v++ = {corners[i][0], corners[i][1]};
        std::memcpy(c, colour, 16);
        c += 16;
    }
}

// ---- crosshair ---------------------------------------------------------------------------------------------
// The crosshair is drawn in the centre of the viewport as untextured triangles, in the same batch as ESP.
// Sizes and gaps are in pixels at 480p (`unit` brings them to the render resolution), so the crosshair scales
// with the picture instead of pixelating.
//
// Each arm is one WriteSegment: a rectangle from `gap` to `gap + size` along its direction. Directions are
// unit vectors; diagonals use 0.707 = cos(45°). The dot uses one square in the middle.
constexpr float kDiag = 0.70710678f;

// one arm of the crosshair: from (cx + dx*gap, cy + dy*gap) to (cx + dx*(gap+size), cy + dy*(gap+size))
void DrawArm(Vertex*& v, uint8_t*& c, float cx, float cy, float dx, float dy, float gap, float size, float thick,
             const float colour[4]) {
    float x0 = cx + dx * gap;
    float y0 = cy + dy * gap;
    float x1 = cx + dx * (gap + size);
    float y1 = cy + dy * (gap + size);
    WriteSegment(v, c, x0, y0, x1, y1, thick, colour);
}

void DrawCrosshairSegments(Vertex*& v, uint8_t*& c, float cx, float cy, float unit) {
    const game::Options& opts = game::Opt();
    float size = float(opts.crosshair_size) * unit;
    float thick = float(opts.crosshair_thickness) * unit;
    float gap = float(opts.crosshair_gap) * unit;
    float colour[4];
    CurrentCrosshairColour(colour);

    switch (opts.crosshair_style) {
        case 0:  // + (cross): 4 axis arms
            DrawArm(v, c, cx, cy, 1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, -1, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, 1, gap, size, thick, colour);
            break;
        case 1:  // * (star): 4 axis arms + 4 diagonal arms
            DrawArm(v, c, cx, cy, 1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, -1, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, 1, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, kDiag, -kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, -kDiag, gap, size, thick, colour);
            break;
        case 2:  // X: 4 diagonal arms
            DrawArm(v, c, cx, cy, kDiag, -kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, -kDiag, gap, size, thick, colour);
            break;
        case 3: {  // DOT: one small filled square
            float s = std::max(thick * 0.5f, 1.0f * unit);
            WriteSegment(v, c, cx, cy - s, cx, cy + s, s * 2, colour);
            break;
        }
        default:
            break;
    }
}

}  // namespace

namespace game {

void EspCamera() {
    uint32_t transforms = SYM(D3D_Device) + XDK_CDevice_Transforms;
    std::memcpy(g_view, HostPtr(transforms), sizeof(g_view));
    std::memcpy(g_projection, HostPtr(transforms + 64), sizeof(g_projection));
    g_camera = true;
    g_camera_frame++;
}

void Aimbot(X86Context& ctx) {
    static uint32_t turned;
    uint32_t c = ctx.esi;
    if (!Opt().aimbot || !g_camera || !R32(kWorld) || turned == g_camera_frame) return;
    turned = g_camera_frame;
    float pull = std::clamp(Opt().aimbot_pull, 1u, 100u) * 0.01f;
    float deadzone = std::clamp(Opt().aimbot_deadzone, 0u, 300u) * 0.1f;
    uint32_t mode = Opt().aimbot_target < 4 ? Opt().aimbot_target : 0;
    float target_y = kAimHeights[mode];
    auto now = std::chrono::steady_clock::now();
    float best = kCone, yaw = 0, pitch = 0;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || R32(who + kCharKind) != kEnemyKind || R32(who + kCharDying)) continue;
        const float* at = &G<float>(who + kCharPosition);
        float aim[3] = {at[0], at[1] + target_y, at[2]}, clip[4];
        if (!Clip(aim, clip)) continue;
        float y = -std::atan(clip[0] / clip[3] / g_projection[0]) * kDegrees;
        float p = -std::atan(clip[1] / clip[3] / g_projection[5]) * kDegrees;
        float off = std::sqrt(y * y + p * p);
        if (off < best) best = off, yaw = y, pitch = p;
    }
    if (best < deadzone) return;
    G<float>(c + kControllerYaw) += yaw * pull;
    G<float>(c + kControllerPitch) += pitch * pull;
}

// D3DVIEWPORT8: x, y, width, height. Called between the 2D layer's begin and end. Bit flags in esp_display:
// bit 0 = line, bit 1 = health bar. Draw order: line first, then the health bar above the head.
// The crosshair (options.h) is drawn FIRST, before the ESP guard: it appears even when ESP is off, and its
// colour scheme (crosshair_color / crosshair_red / crosshair_green / crosshair_blue) is independent of ESP's.
void DrawCrosshair(X86Context& ctx, float unit) {
    const game::Options& opts = Opt();
    if (!opts.crosshair || !R32(kWorld)) return;
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
    float cx = vx + vw * 0.5f;
    float cy = vy + vh * 0.5f;
    static const uint32_t buffer = mem::AllocSystem(kCrosshairMaxVerts * sizeof(Vertex), 16);
    static const uint32_t offset = mem::AllocSystem(8, 16);
    static const uint32_t colours = mem::AllocSystem(kCrosshairMaxVerts * sizeof(float) * 4, 16);
    Vertex* first = static_cast<Vertex*>(HostPtr(buffer));
    Vertex* v = first;
    uint8_t* c = static_cast<uint8_t*>(HostPtr(colours));
    DrawCrosshairSegments(v, c, cx, cy, unit);
    if (v == first) return;
    W32(offset, 0);
    W32(offset + 4, 0);
    uint32_t eax = ctx.eax, ecx = ctx.ecx;
    ctx.eax = uint32_t(v - first) / 3;
    ctx.ecx = buffer;
    CallGuest(ctx, kTriangles, {offset, colours});
    ctx.eax = eax;
    ctx.ecx = ecx;
}

void DrawEsp(X86Context& ctx, float unit) {
    const game::Options& opts = Opt();
    // crosshair draws first, before the ESP guard: independent of the ESP master toggle
    if (opts.crosshair) DrawCrosshair(ctx, unit);
    if (!opts.esp || !g_camera || !R32(kWorld)) return;
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
    // the vertex buffer, its call offset, and the per-vertex colour array: 16 bytes (4 floats) for each of
    // the kMaxVertices vertices. Colours are written per-vertex as vertices are emitted, so the health bar
    // can use its own green/yellow/red palette while the line and the bar's background keep the user's
    // ESP colour.
    static const uint32_t buffer = mem::AllocSystem(kMaxVertices * sizeof(Vertex), 16);
    static const uint32_t offset = mem::AllocSystem(8, 16);
    static const uint32_t colours = mem::AllocSystem(kMaxVertices * sizeof(float) * 4, 16);
    float espColour[4];
    CurrentColour(espColour);

    Vertex* first = static_cast<Vertex*>(HostPtr(buffer));
    Vertex* v = first;
    uint8_t* c = static_cast<uint8_t*>(HostPtr(colours));
    float width = kThickness * unit;
    auto now = std::chrono::steady_clock::now();
    // the display mode: bit 0 = line, bit 1 = health bar (mask to 2 bits so an old config with 4-7 still
    // decodes to the same visible elements rather than to garbage)
    uint32_t display = opts.esp_display & 0x3;
    bool show_line = (display & 1) != 0;
    bool show_health = (display & 2) != 0;
    size_t kept = 0;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || R32(who + kCharKind) != kEnemyKind || R32(who + kCharDying)) continue;
        g_seen[kept++] = g_seen[i];
        if (!show_line && !show_health) continue;
        const float* at = &G<float>(who + kCharPosition);
        float feet[3] = {at[0], at[1], at[2]};
        float head[3] = {at[0], at[1] + kHeight, at[2]};
        float f[2], h[2];
        if (!Project(feet, vx, vy, vw, vh, f) || !Project(head, vx, vy, vw, vh, h)) continue;
        // the box width in screen space — used to size the health bar consistently across distances
        float full_h_screen = f[1] - h[1];
        float char_width_screen = full_h_screen * kBoxWidth;
        float half = char_width_screen * 0.5f;
        float left = f[0] - half, right = f[0] + half;
        if (show_line) {
            WriteSegment(v, c, vx + vw * 0.5f, vy + vh, f[0], f[1], width, espColour);
        }
        if (show_health) {
            float barHeight = full_h_screen * 0.08f;
            float barY = h[1] - barHeight - full_h_screen * 0.02f;
            float barLeft = left;
            float barRight = right;
            float barWidth = barRight - barLeft;
            float hp = G<float>(who + kCharHealth);
            if (!std::isfinite(hp) || hp < 0) hp = 0;
            float ratio = std::min(hp / kEnemyRefHealth, 1.0f);
            float bgColour[4] = {espColour[0] * 0.3f, espColour[1] * 0.3f, espColour[2] * 0.3f, 0.85f};
            WriteSegment(v, c, barLeft, barY, barRight, barY, barHeight, bgColour);
            if (ratio > 0.001f) {
                const float* healthFill = ratio > 0.66f ? kHealthColourHigh
                                      : ratio > 0.33f ? kHealthColourMid
                                      : kHealthColourLow;
                float fillRight = barLeft + barWidth * ratio;
                WriteSegment(v, c, barLeft, barY, fillRight, barY, barHeight, healthFill);
            }
        }
    }
    g_count = kept;
    if (v == first) return;
    W32(offset, 0);
    W32(offset + 4, 0);
    uint32_t eax = ctx.eax, ecx = ctx.ecx;
    ctx.eax = uint32_t(v - first) / 3;
    ctx.ecx = buffer;
    CallGuest(ctx, kTriangles, {offset, colours});
    ctx.eax = eax;
    ctx.ecx = ecx;
}

}  // namespace game

X86_EXTERN_FUNC(__imp__sub_001DFD90);
X86_FUNC(sub_001DFD90) {
    uint32_t who = ctx.ecx;
    if ((game::Opt().esp || game::Opt().aimbot) && who && R32(who + kCharKind) == kEnemyKind && !R32(who + kCharDying))
        Remember(who);
    __imp__sub_001DFD90(ctx, base);
}
