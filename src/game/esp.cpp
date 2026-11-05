// The cheats page's ESP: each living enemy can show a box round it, a line to it from the bottom of the screen,
// a health bar above its head, and/or a body silhouette (tinted quads following the character's head/torso/leg
// proportions). What shows is picked by esp_display's bits (0-15 = base modes with box/line/health/body,
// 16-23 = x-ray variants where the body silhouette is brightened with a bright spine). The body bit is
// implied on whenever the x-ray bit is set, so values 24-31 (should they exist in older configs) are remapped
// to 16-23 by the renderer. esp_fill is a separate toggle for the whole head-to-feet rectangular fill. All
// elements use the colour picked by esp_color (preset or esp_red / esp_green / esp_blue). Health bar fill goes
// green above 66% of the reference max, yellow above 33%, red below. AIMBOT: the view turns to the point on
// the living enemy nearest the crosshair chosen by aimbot_target (neck, body, legs or head), by aimbot_pull
// each frame, but never inside aimbot_deadzone.
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

// fill overlay alpha: 0.30 = 30% opacity so the enemy silhouette stays visible through the tint. When the
// x-ray bit is set on esp_display, the body silhouette uses kXrayBodyAlpha instead so the enemy reads as
// "seen through" the wall.
constexpr float kFillAlpha = 0.30f;
constexpr float kXrayBodyAlpha = 0.55f;

// The body silhouette is drawn as 8 stacked filled quads (head → feet). Each segment has a bottom_frac and
// top_frac (relative to kHeight, 0 = feet, 1.0 = top of head) and a width_frac (relative to the character's
// full box width at that height). Widths vary so the silhouette follows a rough human contour.
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
//   fill (whole box)  : 6
//   body silhouette   : 8 segments × 6 = 48
//   body spine (xray) : 6
//   line              : 6
//   box outline       : 24 (4 sides × 6)
//   health bar        : 12 (bg + fill)
//   total max         : 102 → rounded to 128
constexpr size_t kVertsPerEnemy = 128;
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

// Writes one filled quad (6 vertices, 2 triangles).
void WriteQuad(Vertex*& v, uint8_t*& c, float x0, float y0, float x1, float y1, const float colour[4]) {
    const float corners[4][2] = {{x0, y0}, {x0, y1}, {x1, y1}, {x1, y0}};
    for (int i : {0, 1, 2, 0, 2, 3}) {
        *v++ = {corners[i][0], corners[i][1]};
        std::memcpy(c, colour, 16);
        c += 16;
    }
}

// Writes one rectangle outline (4 sides, 24 vertices).
void WriteRect(Vertex*& v, uint8_t*& c, float sx_left, float sx_right, float sy_top, float sy_bottom,
               float width, const float colour[4]) {
    WriteSegment(v, c, sx_left,  sy_top,    sx_right, sy_top,    width, colour);
    WriteSegment(v, c, sx_left,  sy_bottom, sx_right, sy_bottom, width, colour);
    WriteSegment(v, c, sx_left,  sy_top,    sx_left,  sy_bottom, width, colour);
    WriteSegment(v, c, sx_right, sy_top,    sx_right, sy_bottom, width, colour);
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

// D3DVIEWPORT8: x, y, width, height. Called between the 2D layer's begin and end. Mode bits in esp_display:
// bit 0 = box, bit 1 = line, bit 2 = health bar, bit 3 = body silhouette, bit 4 = x-ray (implies body).
// Draw order (so outlines stay crisp on top of fills):
//   1. whole-box fill (esp_fill) — rectangular tint over the head-to-feet area
//   2. body silhouette — normal (alpha 30%) or x-ray (alpha 55% + bright spine)
//   3. line — from the bottom of the screen to the feet
//   4. box outline — rectangular outline
//   5. health bar — above the head, with its own green/yellow/red palette for the fill
void DrawEsp(X86Context& ctx, float unit) {
    const game::Options& opts = Opt();
    if (!opts.esp || !g_camera || !R32(kWorld)) return;
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
    static const uint32_t buffer = mem::AllocSystem(kMaxVertices * sizeof(Vertex), 16);
    static const uint32_t offset = mem::AllocSystem(8, 16);
    static const uint32_t colours = mem::AllocSystem(kMaxVertices * sizeof(float) * 4, 16);
    float espColour[4];
    CurrentColour(espColour);
    float fillColour[4] = {espColour[0], espColour[1], espColour[2], kFillAlpha};

    Vertex* first = static_cast<Vertex*>(HostPtr(buffer));
    Vertex* v = first;
    uint8_t* c = static_cast<uint8_t*>(HostPtr(colours));
    float width = kThickness * unit;
    auto now = std::chrono::steady_clock::now();
    // decode mode bits; any 24-31 value from an older config is remapped to the equivalent 16-23 (body
    // always implied when xray is set, so a "24" is the same as "16", a "25" as "17", etc.)
    uint32_t display = opts.esp_display;
    if (display >= 24) display = 16 + (display & 0x7);
    bool show_box = (display & 1) != 0;
    bool show_line = (display & 2) != 0;
    bool show_health = (display & 4) != 0;
    bool show_xray = (display & 16) != 0;
    // xray implies body so the body silhouette is always shown when the x-ray bit is set
    bool show_body = (display & 8) != 0 || show_xray;
    bool show_fill = opts.esp_fill;
    // x-ray body: brighter colour, higher alpha; spine brightened 30% + 0.15 per channel, clamped to 1.0
    float xrayBody[4] = {espColour[0], espColour[1], espColour[2], kXrayBodyAlpha};
    float xraySpine[4] = {
        std::min(espColour[0] * 1.3f + 0.15f, 1.0f),
        std::min(espColour[1] * 1.3f + 0.15f, 1.0f),
        std::min(espColour[2] * 1.3f + 0.15f, 1.0f),
        0.95f,
    };
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
        float full_h_screen = f[1] - h[1];
        float char_width_screen = full_h_screen * kBoxWidth;
        float half = char_width_screen * 0.5f;
        float left = f[0] - half, right = f[0] + half;
        if (show_fill) {
            WriteQuad(v, c, left, h[1], right, f[1], fillColour);
        }
        if (show_body) {
            const float* bodyColour = show_xray ? xrayBody : fillColour;
            for (const BodySegment& seg : kBodySegments) {
                float wb = at[1] + kHeight * seg.bottom_frac;
                float wt = at[1] + kHeight * seg.top_frac;
                float wp_b[3] = {at[0], wb, at[2]};
                float wp_t[3] = {at[0], wt, at[2]};
                float sb[2], st[2];
                if (!Project(wp_b, vx, vy, vw, vh, sb)) continue;
                if (!Project(wp_t, vx, vy, vw, vh, st)) continue;
                if (sb[1] - st[1] < 0.5f) continue;
                float seg_half = char_width_screen * 0.5f * seg.width_frac;
                float sl = sb[0] - seg_half, sr = sb[0] + seg_half;
                WriteQuad(v, c, sl, st[1], sr, sb[1], bodyColour);
            }
            // x-ray adds a bright vertical spine down the middle of the enemy
            if (show_xray) {
                float spine_top[3] = {at[0], at[1] + kHeight * 0.97f, at[2]};
                float spine_bot[3] = {at[0], at[1] + 0.03f, at[2]};
                float sp_top[2], sp_bot[2];
                if (Project(spine_top, vx, vy, vw, vh, sp_top) &&
                    Project(spine_bot, vx, vy, vw, vh, sp_bot)) {
                    WriteSegment(v, c, sp_top[0], sp_top[1], sp_bot[0], sp_bot[1], width * 1.8f, xraySpine);
                }
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

X86_EXTERN_FUNC(__imp__sub_001DFD90);
X86_FUNC(sub_001DFD90) {
    uint32_t who = ctx.ecx;
    if ((game::Opt().esp || game::Opt().aimbot) && who && R32(who + kCharKind) == kEnemyKind && !R32(who + kCharDying))
        Remember(who);
    __imp__sub_001DFD90(ctx, base);
}
