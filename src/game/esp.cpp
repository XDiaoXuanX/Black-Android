// The cheats page's ESP: each living enemy can show a line from the bottom of the screen to its feet and/or a
// health bar above its head. What shows is picked by esp_display's two bits (bit 0 = line, bit 1 = health bar),
// so 4 combinations from 0 (nothing) to 3 (line + health) are available from a single selector. The line and
// the health bar's background use the colour picked by esp_color (preset or esp_red / esp_green / esp_blue);
// the health bar's fill uses its own green/yellow/red palette (green above 66% of the reference max, yellow
// above 33%, red below). AIMBOT: the view turns to the point on the living enemy nearest the crosshair chosen
// by aimbot_target (neck, body, legs or head), by aimbot_pull each frame, but never inside aimbot_deadzone.
//
// ESP DISTANCE (esp_distance): shows the enemy's distance from the player as a small 3-digit number above the
// health bar (or above the head if the health bar is off). Drawn with 7-segment digits made from the same
// untextured triangle batch as everything else, in the ESP colour. The number is the 3D distance in metres,
// capped at 999 to fit the 3-digit display.
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

// aimbot target heights above the feet, chosen by aimbot_target
constexpr float kAimHeights[4] = {
    1.55f,  // NECK (default)
    1.15f,  // BODY
    0.45f,  // LEGS
    1.68f,  // HEAD
};

// the ESP line colours the preset row offers
struct EspPreset {
    float r, g, b;
};
constexpr EspPreset kEspPresets[] = {
    {0.88f, 0.86f, 0.82f},  // WHITE
    {1.00f, 0.20f, 0.20f},  // RED
    {0.20f, 1.00f, 0.30f},  // GREEN
    {0.20f, 0.90f, 1.00f},  // CYAN
    {1.00f, 0.85f, 0.20f},  // YELLOW
    {1.00f, 0.30f, 1.00f},  // MAGENTA
    {1.00f, 0.55f, 0.10f},  // ORANGE
    {0.70f, 0.40f, 1.00f},  // PURPLE
};
constexpr size_t kEspPresetCount = sizeof(kEspPresets) / sizeof(kEspPresets[0]);

constexpr float kCone = 30.0f;
constexpr float kDegrees = 180.0f / 3.14159265f;

constexpr auto kSeen = std::chrono::milliseconds(250);
constexpr size_t kMaxEnemies = 128;

constexpr float kEnemyRefHealth = 250.0f;

constexpr float kHealthColourHigh[4] = {0.20f, 0.90f, 0.20f, 1.0f};
constexpr float kHealthColourMid[4] = {1.00f, 0.85f, 0.10f, 1.0f};
constexpr float kHealthColourLow[4] = {1.00f, 0.20f, 0.20f, 1.0f};

struct Seen {
    uint32_t who;
    std::chrono::steady_clock::time_point at;
};
Seen g_seen[kMaxEnemies];
size_t g_count;

float g_view[16], g_projection[16];
bool g_camera;
uint32_t g_camera_frame;

constexpr uint32_t kTriangles = 0xD5A00;
struct Vertex {
    float x, y;
};

// Per-enemy vertex budget:
//   line         : 6   (2 triangles)
//   health bar   : 12  (background + fill, 2 triangles each)
//   distance     : up to 3 digits × 7 segments × 6 verts = 126
//   total max    : 144 → rounded to 200 for safety
constexpr size_t kVertsPerEnemy = 200;
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

bool Clip(const float p[3], float clip[4]) {
    float eye[4];
    for (int c = 0; c < 4; c++)
        eye[c] = p[0] * g_view[c] + p[1] * g_view[4 + c] + p[2] * g_view[8 + c] + g_view[12 + c];
    for (int c = 0; c < 4; c++)
        clip[c] = eye[0] * g_projection[c] + eye[1] * g_projection[4 + c] + eye[2] * g_projection[8 + c] +
                  eye[3] * g_projection[12 + c];
    return clip[3] > kNear;
}

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

// ---- 7-segment digit rendering (used for ESP DISTANCE) ---------------------------------------------------
//
// Each digit is a standard 7-segment layout drawn as rectangles:
//    ---A---
//   |       |
//   F       B
//   |       |
//    ---G---
//   |       |
//   E       C
//   |       |
//    ---D---
//
// Segment mask bits:
//   bit 0 = A (top)
//   bit 1 = B (top-right)
//   bit 2 = C (bottom-right)
//   bit 3 = D (bottom)
//   bit 4 = E (bottom-left)
//   bit 5 = F (top-left)
//   bit 6 = G (middle)
constexpr uint8_t kDigitSegments[10] = {
    0x3F,  // 0 = A B C D E F
    0x06,  // 1 = B C
    0x5B,  // 2 = A B D E G
    0x4F,  // 3 = A B C D G
    0x66,  // 4 = B C F G
    0x6D,  // 5 = A C D F G
    0x7D,  // 6 = A C D E F G
    0x07,  // 7 = A B C
    0x7F,  // 8 = A B C D E F G
    0x6F,  // 9 = A B C D F G
};

// Draws one digit in the cell (x, y) to (x + w, y + h), segment thickness = t.
void DrawDigit(Vertex*& v, uint8_t*& c, float x, float y, float w, float h, int digit, float t,
               const float colour[4]) {
    if (digit < 0 || digit > 9) return;
    uint8_t mask = kDigitSegments[digit];
    // A: top horizontal
    if (mask & 0x01) WriteSegment(v, c, x + t, y + t * 0.5f, x + w - t, y + t * 0.5f, t, colour);
    // B: top-right vertical
    if (mask & 0x02) WriteSegment(v, c, x + w - t * 0.5f, y + t, x + w - t * 0.5f, y + h * 0.5f - t * 0.5f, t, colour);
    // C: bottom-right vertical
    if (mask & 0x04) WriteSegment(v, c, x + w - t * 0.5f, y + h * 0.5f + t * 0.5f, x + w - t * 0.5f, y + h - t, t, colour);
    // D: bottom horizontal
    if (mask & 0x08) WriteSegment(v, c, x + t, y + h - t * 0.5f, x + w - t, y + h - t * 0.5f, t, colour);
    // E: bottom-left vertical
    if (mask & 0x10) WriteSegment(v, c, x + t * 0.5f, y + h * 0.5f + t * 0.5f, x + t * 0.5f, y + h - t, t, colour);
    // F: top-left vertical
    if (mask & 0x20) WriteSegment(v, c, x + t * 0.5f, y + t, x + t * 0.5f, y + h * 0.5f - t * 0.5f, t, colour);
    // G: middle horizontal
    if (mask & 0x40) WriteSegment(v, c, x + t, y + h * 0.5f, x + w - t, y + h * 0.5f, t, colour);
}

// Draws an integer (0..999) centred at cx horizontally, with the bottom edge at bottom_y.
void DrawNumber(Vertex*& v, uint8_t*& c, float cx, float bottom_y, float w, float h, int number, float t,
                const float colour[4]) {
    int digits[3];
    int n = 0;
    int val = number < 0 ? 0 : (number > 999 ? 999 : number);
    if (val == 0) {
        digits[n++] = 0;
    } else {
        while (val > 0 && n < 3) {
            digits[n++] = val % 10;
            val /= 10;
        }
    }
    float total_w = float(n) * w + float(n - 1) * t;
    float x = cx - total_w * 0.5f;
    float y = bottom_y - h;
    for (int i = n - 1; i >= 0; i--) {
        DrawDigit(v, c, x, y, w, h, digits[i], t, colour);
        x += w + t;
    }
}

// ---- crosshair ---------------------------------------------------------------------------------------------
constexpr float kDiag = 0.70710678f;

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
        case 0:  // +
            DrawArm(v, c, cx, cy, 1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, -1, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, 1, gap, size, thick, colour);
            break;
        case 1:  // *
            DrawArm(v, c, cx, cy, 1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, -1, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, 1, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, kDiag, -kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, -kDiag, gap, size, thick, colour);
            break;
        case 2:  // X
            DrawArm(v, c, cx, cy, kDiag, -kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, -kDiag, gap, size, thick, colour);
            break;
        case 3: {  // DOT
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
    // the kMaxVertices vertices. Colours are written per-vertex as vertices are emitted.
    static const uint32_t buffer = mem::AllocSystem(kMaxVertices * sizeof(Vertex), 16);
    static const uint32_t offset = mem::AllocSystem(8, 16);
    static const uint32_t colours = mem::AllocSystem(kMaxVertices * sizeof(float) * 4, 16);
    float espColour[4];
    CurrentColour(espColour);

    // the player's world position, for the distance display
    uint32_t me = R32(kWorld) + kWorldPlayer;
    const float* myPos = &G<float>(me + kCharPosition);

    Vertex* first = static_cast<Vertex*>(HostPtr(buffer));
    Vertex* v = first;
    uint8_t* c = static_cast<uint8_t*>(HostPtr(colours));
    float width = kThickness * unit;
    auto now = std::chrono::steady_clock::now();
    uint32_t display = opts.esp_display & 0x3;
    bool show_line = (display & 1) != 0;
    bool show_health = (display & 2) != 0;
    bool show_distance = opts.esp_distance;
    size_t kept = 0;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || R32(who + kCharKind) != kEnemyKind || R32(who + kCharDying)) continue;
        g_seen[kept++] = g_seen[i];
        if (!show_line && !show_health && !show_distance) continue;
        const float* at = &G<float>(who + kCharPosition);
        float feet[3] = {at[0], at[1], at[2]};
        float head[3] = {at[0], at[1] + kHeight, at[2]};
        float f[2], h[2];
        if (!Project(feet, vx, vy, vw, vh, f) || !Project(head, vx, vy, vw, vh, h)) continue;
        float full_h_screen = f[1] - h[1];
        float char_width_screen = full_h_screen * kBoxWidth;
        float half = char_width_screen * 0.5f;
        float left = f[0] - half, right = f[0] + half;
        if (show_line) {
            WriteSegment(v, c, vx + vw * 0.5f, vy + vh, f[0], f[1], width, espColour);
        }
        float barY = 0.0f;
        float barHeight = 0.0f;
        if (show_health) {
            barHeight = full_h_screen * 0.08f;
            barY = h[1] - barHeight - full_h_screen * 0.02f;
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
        if (show_distance) {
            // Compute the enemy's 3D distance from the player in world units (metres).
            float dx = at[0] - myPos[0];
            float dy = at[1] - myPos[1];
            float dz = at[2] - myPos[2];
            int dist = int(std::sqrt(dx * dx + dy * dy + dz * dz) + 0.5f);
            if (dist < 0) dist = 0;
            if (dist > 999) dist = 999;

            // Digit cell size scales with the enemy's on-screen height, clamped so very close and very
            // far enemies both stay legible.
            float num_h = std::clamp(full_h_screen * 0.10f, 6.0f * unit, 32.0f * unit);
            float num_w = num_h * 0.55f;
            float thick_n = std::max(num_h * 0.12f, 1.0f * unit);

            // Bottom of the number: above the health bar if shown, else just above the head.
            float bottom_y;
            if (show_health) {
                bottom_y = barY - barHeight * 0.5f - num_h * 0.15f;
            } else {
                bottom_y = h[1] - num_h * 0.15f;
            }

            DrawNumber(v, c, f[0], bottom_y, num_w, num_h, dist, thick_n, espColour);
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
