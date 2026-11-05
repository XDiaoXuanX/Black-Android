// The cheats page's ESP: each living enemy or friendly gets a box round it and a line to it from the bottom of
// the screen, drawn over the finished frame with the view the scene was drawn with, in the colour chosen by
// esp_color (a preset or the esp_red / esp_green / esp_blue custom rgb). The target can be picked by esp_targets
// (ENEMIES / FRIENDLIES / BOTH); friendlies draw in a fixed blue when both are shown, so they are visually
// distinguishable from enemies, and in the user's ESP colour when FRIENDLIES mode is selected alone. esp_display
// picks the shape (BOTH / BOX / LINE). esp_health shows a health bar above each head: green above 66% of the
// reference max, yellow above 33%, red below.
//
// AIMBOT: the view turns to the point on the chosen target nearest the crosshair, by aimbot_pull each frame, but
// never inside aimbot_deadzone. The aimbot has two separate filters:
//   - aimbot_targets: which character kind to aim at (ENEMIES / FRIENDLIES / BOTH), sharing esp_friendly_kind
//     with the ESP filter so it only needs to be tuned once. Independent of esp_targets.
//   - aimbot_target:  which body part to aim at (NECK / BODY / LEGS / HEAD).
// When aimbot_targets is set to BOTH, an ally running into the cone can steal the crosshair from a distant
// enemy — keep it on ENEMIES if that is undesirable.
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
constexpr uint32_t kCharHealth = 0x2F0;   // float, the character's current health (enemy and player alike)
constexpr uint32_t kCharPosition = 0xA0;
constexpr uint32_t kCharDying = 0x384;
constexpr uint32_t kEnemyKind = 1;
// kFriendlyKind is a setting (esp_friendly_kind), so which value counts as friendly can be tuned from the panel
// if 3 does not match the game's character-kind convention.

// the player's controller: yaw positive left, pitch positive down, in degrees (mouse.cpp)
constexpr uint32_t kControllerYaw = 0x18;
constexpr uint32_t kControllerPitch = 0x1C;

constexpr float kHeight = 1.8f;      // world units are metres: the near plane is 0.08
constexpr float kBoxWidth = 0.45f;   // of the box's height
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

// fixed secondary colour used for friendlies when BOTH targets are shown, so they are distinguishable from
// enemies (which use the user's chosen ESP colour). Light blue/cyan: stands out clearly against warm ESP
// colours like orange/red/yellow, and is not one of the presets so it will not clash.
constexpr float kFriendlyColour[4] = {0.30f, 0.70f, 1.00f, 1.0f};

constexpr float kCone = 30.0f;       // degrees from the crosshair a target is taken in
constexpr float kDegrees = 180.0f / 3.14159265f;

// a target is drawn while its update ran this recently: none once the game pauses, no flicker between ticks
constexpr auto kSeen = std::chrono::milliseconds(250);
constexpr size_t kMaxEnemies = 128;

// Health bar reference: a target with this much health or more shows a full bar. Common Call of Duty Classic
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

// Per-target vertex budget: line (6) + 4 box sides (24) + health background (6) + health fill (6) = 42 verts.
// Allocating for the maximum keeps the buffers a fixed size regardless of the display mode.
constexpr size_t kVertsPerEnemy = 42;
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
// is read off the projection, the angles of the target from the middle of the view. The target kind is chosen by
// aimbot_targets (ENEMIES / FRIENDLIES / BOTH, sharing esp_friendly_kind with the ESP filter); the body part by
// aimbot_target (NECK / BODY / LEGS / HEAD); the strength by aimbot_pull; and the deadzone inside which no turn
// is applied by aimbot_deadzone — so the player chooses both the magnet and how freely the crosshair slides from
// one target to the next. Works zoomed or not: no zoom gate.
void Aimbot(X86Context& ctx) {
    static uint32_t turned;
    uint32_t c = ctx.esi;
    const game::Options& o = Opt();
    if (!o.aimbot || !g_camera || !R32(kWorld) || turned == g_camera_frame) return;
    turned = g_camera_frame;
    // the tunables, read once a frame and clamped so a corrupt setting still aims somewhere sane:
    //   targets:  0 enemies, 1 friendlies, 2 both
    //   friendly: which kind counts as a friendly (shared with the ESP filter)
    //   body:     which body part to aim at on the chosen target (NECK / BODY / LEGS / HEAD)
    //   pull:     1% to 100% of the angle to the target, applied this frame
    //   deadzone: 0.0° to 30.0°, inside which the aimbot does not turn at all
    uint32_t targets = o.aimbot_targets < 3 ? o.aimbot_targets : 0;
    uint32_t friendly_kind = o.esp_friendly_kind;
    uint32_t body = o.aimbot_target < 4 ? o.aimbot_target : 0;
    float target_y = kAimHeights[body];
    float pull = std::clamp(o.aimbot_pull, 1u, 100u) * 0.01f;
    float deadzone = std::clamp(o.aimbot_deadzone, 0u, 300u) * 0.1f;
    auto now = std::chrono::steady_clock::now();
    float best = kCone, yaw = 0, pitch = 0;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || R32(who + kCharDying)) continue;
        uint32_t kind = R32(who + kCharKind);
        bool is_enemy = (kind == kEnemyKind);
        bool is_friendly = (kind == friendly_kind);
        // aimbot target filter
        bool wanted = (targets == 0 && is_enemy)
                   || (targets == 1 && is_friendly)
                   || (targets == 2 && (is_enemy || is_friendly));
        if (!wanted) continue;
        const float* at = &G<float>(who + kCharPosition);
        float aim[3] = {at[0], at[1] + target_y, at[2]}, clip[4];
        if (!Clip(aim, clip)) continue;
        float y = -std::atan(clip[0] / clip[3] / g_projection[0]) * kDegrees;
        float p = -std::atan(clip[1] / clip[3] / g_projection[5]) * kDegrees;
        float off = std::sqrt(y * y + p * p);
        if (off < best) best = off, yaw = y, pitch = p;
    }
    // already on the target, or nothing within the cone: leave the crosshair to the player so it can be slid
    // onto another target without the assist dragging it back
    if (best < deadzone) return;
    G<float>(c + kControllerYaw) += yaw * pull;
    G<float>(c + kControllerPitch) += pitch * pull;
}

// D3DVIEWPORT8: x, y, width, height. Called between the 2D layer's begin and end
void DrawEsp(X86Context& ctx, float unit) {
    const game::Options& opts = Opt();
    if (!opts.esp || !g_camera || !R32(kWorld)) return;
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
    // the vertex buffer, its call offset, and the per-vertex colour array: 16 bytes (4 floats) for each of the
    // kMaxVertices vertices. Colours are written per-vertex as vertices are emitted, so a health bar can use its
    // own green/yellow/red palette while the box and line keep the user's ESP colour.
    static const uint32_t buffer = mem::AllocSystem(kMaxVertices * sizeof(Vertex), 16);
    static const uint32_t offset = mem::AllocSystem(8, 16);
    static const uint32_t colours = mem::AllocSystem(kMaxVertices * sizeof(float) * 4, 16);
    // the ESP colour the user chose, as rgba
    float espColour[4];
    CurrentColour(espColour);

    Vertex* first = static_cast<Vertex*>(HostPtr(buffer));
    Vertex* v = first;
    uint8_t* c = static_cast<uint8_t*>(HostPtr(colours));
    float width = kThickness * unit;
    auto now = std::chrono::steady_clock::now();
    // display: 0 = BOTH, 1 = BOX, 2 = LINE. Health bar draws on top, gated by esp_health.
    uint32_t display = opts.esp_display < 3 ? opts.esp_display : 0;
    bool show_box = (display == 0 || display == 1);
    bool show_line = (display == 0 || display == 2);
    bool show_health = opts.esp_health;
    // target filter: 0 = enemies, 1 = friendlies, 2 = both
    uint32_t targets = opts.esp_targets < 3 ? opts.esp_targets : 0;
    uint32_t friendly_kind = opts.esp_friendly_kind;
    size_t kept = 0;
    for (size_t i = 0; i < g_count; i++) {
        uint32_t who = g_seen[i].who;
        if (now - g_seen[i].at > kSeen || R32(who + kCharDying)) continue;
        uint32_t kind = R32(who + kCharKind);
        bool is_enemy = (kind == kEnemyKind);
        bool is_friendly = (kind == friendly_kind);
        bool want_draw = false;
        if (targets == 0) want_draw = is_enemy;
        else if (targets == 1) want_draw = is_friendly;
        else want_draw = is_enemy || is_friendly;
        if (!want_draw) continue;
        g_seen[kept++] = g_seen[i];
        const float* at = &G<float>(who + kCharPosition);
        float feet[3] = {at[0], at[1], at[2]};
        float head[3] = {at[0], at[1] + kHeight, at[2]};
        float f[2], h[2];
        if (!Project(feet, vx, vy, vw, vh, f) || !Project(head, vx, vy, vw, vh, h)) continue;
        // pick the colour: in FRIENDLIES mode, the (only drawn) friendlies use the user's ESP colour; in
        // ENEMIES or BOTH, enemies use the user's ESP colour and friendlies use the fixed friendly blue
        const float* draw_colour = (targets == 1) ? espColour
                                : is_enemy        ? espColour
                                                  : kFriendlyColour;
        float half = (f[1] - h[1]) * kBoxWidth * 0.5f;
        float left = f[0] - half, right = f[0] + half;
        if (show_line) {
            WriteSegment(v, c, vx + vw * 0.5f, vy + vh, f[0], f[1], width, draw_colour);
        }
        if (show_box) {
            WriteSegment(v, c, left, h[1], right, h[1], width, draw_colour);
            WriteSegment(v, c, left, f[1], right, f[1], width, draw_colour);
            WriteSegment(v, c, left, h[1], left, f[1], width, draw_colour);
            WriteSegment(v, c, right, h[1], right, f[1], width, draw_colour);
        }
        if (show_health) {
            float boxHeight = f[1] - h[1];
            float barHeight = boxHeight * 0.08f;
            float barY = h[1] - barHeight - boxHeight * 0.02f;
            float barLeft = left;
            float barRight = right;
            float barWidth = barRight - barLeft;
            float hp = G<float>(who + kCharHealth);
            if (!std::isfinite(hp) || hp < 0) hp = 0;
            float ratio = std::min(hp / kEnemyRefHealth, 1.0f);
            float bgColour[4] = {draw_colour[0] * 0.3f, draw_colour[1] * 0.3f, draw_colour[2] * 0.3f, 0.85f};
            WriteSegment(v, c, barLeft, barY, barRight, barY, barHeight, bgColour);
            if (ratio > 0.001f) {
                const float* fillColour = ratio > 0.66f ? kHealthColourHigh
                                     : ratio > 0.33f ? kHealthColourMid
                                     : kHealthColourLow;
                float fillRight = barLeft + barWidth * ratio;
                WriteSegment(v, c, barLeft, barY, fillRight, barY, barHeight, fillColour);
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

// The characters' update, a frame (ecx the character, its class's own; the player's is 0x1E5FA0): the entities
// that ran it are filtered against the ESP and aimbot target filters and remembered if wanted by either. Each
// filter has its own scope:
//   - esp: remembers the kinds esp_targets asks for (enemies, friendlies, or both)
//   - aimbot: remembers the kinds aimbot_targets asks for (independent of the ESP filter)
X86_EXTERN_FUNC(__imp__sub_001DFD90);
X86_FUNC(sub_001DFD90) {
    uint32_t who = ctx.ecx;
    if (who && !R32(who + kCharDying)) {
        const game::Options& o = game::Opt();
        uint32_t kind = R32(who + kCharKind);
        uint32_t friendly_kind = o.esp_friendly_kind;
        bool remember = false;
        // aimbot target filter
        if (o.aimbot) {
            uint32_t at = o.aimbot_targets < 3 ? o.aimbot_targets : 0;
            if ((at == 0 && kind == kEnemyKind) ||
                (at == 1 && kind == friendly_kind) ||
                (at == 2 && (kind == kEnemyKind || kind == friendly_kind)))
                remember = true;
        }
        // ESP target filter
        if (o.esp && !remember) {
            uint32_t et = o.esp_targets < 3 ? o.esp_targets : 0;
            if ((et == 0 && kind == kEnemyKind) ||
                (et == 1 && kind == friendly_kind) ||
                (et == 2 && (kind == kEnemyKind || kind == friendly_kind)))
                remember = true;
        }
        if (remember) Remember(who);
    }
    __imp__sub_001DFD90(ctx, base);
}
