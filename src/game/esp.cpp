// The cheats page's ESP: each living enemy can show a box round it, a line to it from the bottom of the screen,
// a health bar above its head — chosen by esp_display's bit flags (bit 0 = box, bit 1 = line, bit 2 = health) —
// plus optional extras that work independently of esp_display: esp_body (body-shaped silhouette fill — head,
// torso, legs — with NO rectangular box outline, so enemies are "detected" by tint alone) and esp_fill (a
// semi-transparent tint over the whole head-to-feet box, rectangular). All use the colour picked by esp_color
// (preset or esp_red / esp_green / esp_blue). The health bar's fill goes green above 66% of the reference max,
// yellow above 33%, red below. AIMBOT: the view turns to the point on the living enemy nearest the crosshair
// chosen by aimbot_target (neck, body, legs or head), by aimbot_pull each frame, but never inside
// aimbot_deadzone.
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
constexpr float kBoxWidth = 0.45f;   // of the box's height
constexpr float kNear = 0.08f;       // clip w nearer than this is behind the camera
constexpr float kThickness = 1.5f;   // lines of a 480 frame

// fill overlay alpha: 0.30 = 30% opacity so the enemy silhouette stays visible through the tint. If the game
// does not apply alpha blending in its 2D path, the fill renders opaque and the user can approximate the same
// effect by picking a dark CUSTOM colour. Both the body silhouette and the whole-box fill use this alpha.
constexpr float kFillAlpha = 0.30f;

// The body silhouette is drawn as 8 stacked filled quads (head → feet). Each segment has a bottom_frac and
// top_frac (relative to kHeight, 0 = feet, 1.0 = top of head) and a width_frac (relative to the character's
// full box width at that height). Widths vary so the silhouette follows a rough human contour: narrow at the
// very top (head), medium at the face and neck, widest at the chest/shoulders, slightly narrower at the waist,
// tapering down through the legs, and narrowest at the feet. The segments abut exactly (no gap, no overlap) so
// the silhouette looks like one continuous tinted shape with no seams. Widths chosen to approximate a real
// standing human silhouette (head ≈ 15% of height, shoulders ≈ 40–45%, hips ≈ 30%, feet ≈ 25%).
struct BodySegment {
    float bottom_frac;
    float top_frac;
    float width_frac;  // relative to the character's full box width
};
constexpr BodySegment kBodySegments[] = {
    {0.94f, 1.00f, 0.35f},  // top of head (narrow)
    {0.88f, 0.94f, 0.55f},  // face
    {0.80f, 0.88f, 0.45f},  // neck / collar (narrower than face)
    {0.60f, 0.80f, 0.95f},  // torso / chest (widest — shoulders and arms)
    {0.50f, 0.60f, 0.90f},  // waist / hips (slightly narrower than chest)
    {0.25f, 0.50f, 0.75f},  // upper legs
    {0.10f, 0.25f, 0.60f},  // lower legs
    {0.00f, 0.10f, 0.55f},  // feet / ankles
};

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
// esp_blue, and the CUSTOM entry reads them straight.
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

// Health bar reference: an enemy with this much health or more shows a full bar. Common Call of Duty Classic
// enemies have roughly 100-250 HP; elites go higher. 250 is a sensible visual reference so a typical soldier
// starts around two-thirds to full, giving room to see it drop.
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
//   fill (whole box)  : 6
//   body silhouette   : 8 segments × 6 = 48
//   line              : 6
//   box outline       : 24 (4 sides × 6)
//   health bar        : 12 (bg + fill)
//   total max         : 96
// Allocating for the maximum keeps the buffers a fixed size regardless of the mode.
constexpr size_t kVertsPerEnemy = 120;
constexpr size_t kMaxVertices = kMaxEnemies * kVertsPerEnemy;

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
    // full: drop the one seen longest ago
    Seen* oldest = g_seen;
    for (Seen& s : g_seen)
        if (s.at < oldest->at) oldest = &s;
    *oldest = {who, now};
}

// fills `out` with the rgba the ESP lines should use: a preset, or the custom rgb divided by 255
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
// Both pointers advance by 6 entries: 6 vertices of 8 bytes, 6 colours of 16 bytes.
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

// Writes one filled quad (6 vertices, 2 triangles) with corners TL(x0,y0), BL(x0,y1), BR(x1,y1), TR(x1,y0).
// Same vertex/colour advance as WriteSegment.
void WriteQuad(Vertex*& v, uint8_t*& c, float x0, float y0, float x1, float y1, const float colour[4]) {
    const float corners[4][2] = {{x0, y0}, {x0, y1}, {x1, y1}, {x1, y0}};
    for (int i : {0, 1, 2, 0, 2, 3}) {
        *v++ = {corners[i][0], corners[i][1]};
        std::memcpy(c, colour, 16);
        c += 16;
    }
}

// Writes one rectangle outline (4 sides, 24 vertices). Used for the box outline bit in esp_display.
void WriteRect(Vertex*& v, uint8_t*& c, float sx_left, float sx_right, float sy_top, float sy_bottom,
               float width, const float colour[4]) {
    WriteSegment(v, c, sx_left,  sy_top,    sx_right, sy_top,    width, colour);  // top edge
    WriteSegment(v, c, sx_left,  sy_bottom, sx_right, sy_bottom, width, colour);  // bottom edge
    WriteSegment(v, c, sx_left,  sy_top,    sx_left,  sy_bottom, width, colour);  // left side
    WriteSegment(v, c, sx_right, sy_top,    sx_right, sy_bottom, width, colour);  // right side
}

}  // namespace

namespace game {

// the device's view and projection as the scene draws them (D3DDevice_SetTransform, view 0 and projection 1)
void EspCamera() {
    uint32_t transforms = SYM(D3D_Device) + XDK_CDevice_Transforms;
    std::memcpy(g_view, HostPtr(transforms), sizeof(g_view));
    std::memcpy(g_projection, HostPtr(transforms + 64), sizeof(g_projection));
    g_camera = true;
    g_camera_frame++;
}

// esi the player's controller in its look update, before the game clamps the pitch and wraps the yaw. The turn
// is read off the projection, the angles of the target from the middle of the view. The target height is picked
// by aimbot_target (NECK / BODY / LEGS / HEAD), the strength of the turn by aimbot_pull, and the deadzone inside
// which no turn is applied at all by aimbot_deadzone — so the player chooses both the magnet and how freely the
// crosshair slides from one enemy to the next. Works zoomed or not: no zoom gate.
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
// bit 0 = box, bit 1 = line, bit 2 = health bar. esp_body and esp_fill are separate toggles. Fill draws first
// (so outlines stay crisp over it), then the body silhouette (no outlines — it is filled in segments that
// follow the enemy's proportions, giving head-to-feet detection without a box), then the box/line, then the
// health bar on top.
void DrawEsp(X86Context& ctx, float unit) {
    const game::Options& opts = Opt();
    if (!opts.esp || !g_camera || !R32(kWorld)) return;
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
    // the vertex buffer, its call offset, and the per-vertex colour array: 16 bytes (4 floats) for each of the
    // kMaxVertices vertices. Colours are written per-vertex as vertices are emitted, so the health bar can use
    // its own green/yellow/red palette while the box/line/body/fill keep the user's ESP colour.
    static const uint32_t buffer = mem::AllocSystem(kMaxVertices * sizeof(Vertex), 16);
    static const uint32_t offset = mem::AllocSystem(8, 16);
    static const uint32_t colours = mem::AllocSystem(kMaxVertices * sizeof(float) * 4, 16);
    // the ESP colour the user chose, as rgba, and its fill variant (same rgb, reduced alpha)
    float espColour[4];
    CurrentColour(espColour);
    float fillColour[4] = {espColour[0], espColour[1], espColour[2], kFillAlpha};

    Vertex* first = static_cast<Vertex*>(HostPtr(buffer));
    Vertex* v = first;
    uint8_t* c = static_cast<uint8_t*>(HostPtr(colours));
    float width = kThickness * unit;
    auto now = std::chrono::steady_clock::now();
    // the display mode: bit 0 = box, bit 1 = line, bit 2 = health bar; body and fill are separate toggles
    uint32_t display = opts.esp_display < 8 ? opts.esp_display : 7;
    bool show_box = (display & 1) != 0;
    bool show_line = (display & 2) != 0;
    bool show_health = (display & 4) != 0;
    bool show_body = opts.esp_body;
    bool show_fill = opts.esp_fill;
    size_t kept = 0;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || R32(who + kCharKind) != kEnemyKind || R32(who + kCharDying)) continue;
        g_seen[kept++] = g_seen[i];
        if (!show_box && !show_line && !show_health && !show_body && !show_fill) continue;
        const float* at = &G<float>(who + kCharPosition);
        float feet[3] = {at[0], at[1], at[2]};
        float head[3] = {at[0], at[1] + kHeight, at[2]};
        float f[2], h[2];
        if (!Project(feet, vx, vy, vw, vh, f) || !Project(head, vx, vy, vw, vh, h)) continue;
        // the enemy's full on-screen box height and full character box width — used by the body silhouette
        // so segment widths scale with the character regardless of the camera's pitch/yaw
        float full_h_screen = f[1] - h[1];
        float char_width_screen = full_h_screen * kBoxWidth;
        float half = char_width_screen * 0.5f;
        float left = f[0] - half, right = f[0] + half;
        // whole-box fill first, so silhouettes and outlines draw on top of it
        if (show_fill) {
            WriteQuad(v, c, left, h[1], right, f[1], fillColour);
        }
        // body silhouette: 8 stacked filled quads with varying widths, tracing a rough human outline from
        // head to feet. No rectangles outside the body; no outlines at all. This is the "detect body without
        // box" mode — the enemy's body area is tinted with the ESP colour at kFillAlpha opacity.
        if (show_body) {
            for (const BodySegment& seg : kBodySegments) {
                float wb = at[1] + kHeight * seg.bottom_frac;  // world Y at segment's bottom
                float wt = at[1] + kHeight * seg.top_frac;     // world Y at segment's top
                float wp_b[3] = {at[0], wb, at[2]};
                float wp_t[3] = {at[0], wt, at[2]};
                float sb[2], st[2];
                if (!Project(wp_b, vx, vy, vw, vh, sb)) continue;
                if (!Project(wp_t, vx, vy, vw, vh, st)) continue;
                if (sb[1] - st[1] < 0.5f) continue;  // segment too tiny to draw
                // width is a fraction of the character's full box width at that height
                float seg_half = char_width_screen * 0.5f * seg.width_frac;
                float sl = sb[0] - seg_half, sr = sb[0] + seg_half;
                WriteQuad(v, c, sl, st[1], sr, sb[1], fillColour);
            }
        }
        if (show_line) {
            WriteSegment(v, c, vx + vw * 0.5f, vy + vh, f[0], f[1], width, espColour);
        }
        if (show_box) {
            WriteRect(v, c, left, right, h[1], f[1], width, espColour);
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

// The characters' update, a frame (ecx the character, its class's own; the player's is 0x1E5FA0): the enemies
// that ran it are the ones drawn
X86_EXTERN_FUNC(__imp__sub_001DFD90);
X86_FUNC(sub_001DFD90) {
    uint32_t who = ctx.ecx;
    if ((game::Opt().esp || game::Opt().aimbot) && who && R32(who + kCharKind) == kEnemyKind && !R32(who + kCharDying))
        Remember(who);
    __imp__sub_001DFD90(ctx, base);
}
