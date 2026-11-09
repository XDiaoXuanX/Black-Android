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
//
// The SCOPE (options.h) is drawn in the same 2D layer when the player is aiming down sights. It consists of
// a black vignette (ring) revealing only a circular area, and a crosshair whose shape depends on scope_style.
// See options.h's SCOPE section for the full documentation. The scope is drawn BEFORE the crosshair, so the
// crosshair appears on top of the scope if both are enabled (though typically the crosshair is hidden while
// scoped).
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

// player / weapon offsets used by the scope detection
constexpr uint32_t kPlayerInventory = 0x278;
constexpr uint32_t kInvHeld = 0x24;
constexpr uint32_t kPlayerZoom = 0x2A4;     // float, the current ADS zoom (1 = hip)
constexpr uint32_t kWeaponData = 0xEC;
constexpr uint32_t kDataAimZoom = 0xB4;     // float, the weapon's aim zoom target

constexpr float kHeight = 1.8f;
constexpr float kBoxWidth = 0.45f;
constexpr float kNear = 0.08f;
constexpr float kThickness = 1.5f;

constexpr float kAimHeights[4] = {1.55f, 1.15f, 0.45f, 1.68f};

struct EspPreset {
    float r, g, b;
};
constexpr EspPreset kEspPresets[] = {
    {0.88f, 0.86f, 0.82f},
    {1.00f, 0.20f, 0.20f},
    {0.20f, 1.00f, 0.30f},
    {0.20f, 0.90f, 1.00f},
    {1.00f, 0.85f, 0.20f},
    {1.00f, 0.30f, 1.00f},
    {1.00f, 0.55f, 0.10f},
    {0.70f, 0.40f, 1.00f},
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

constexpr size_t kVertsPerEnemy = 24;
constexpr size_t kMaxVertices = kMaxEnemies * kVertsPerEnemy;

// Combined overlay buffer (scope + crosshair): 32-segment vignette (192 verts) +
// crosshair up to ~120 verts + margin = 512 verts.
constexpr size_t kCombinedOverlayVerts = 512;

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

void CurrentScopeColour(float out[4]) {
    const game::Options& opts = game::Opt();
    uint32_t preset = opts.scope_color;
    if (preset < kEspPresetCount) {
        out[0] = kEspPresets[preset].r;
        out[1] = kEspPresets[preset].g;
        out[2] = kEspPresets[preset].b;
    } else {
        out[0] = float(opts.scope_red) / 255.0f;
        out[1] = float(opts.scope_green) / 255.0f;
        out[2] = float(opts.scope_blue) / 255.0f;
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

// ---- crosshair -------------------------------------------------------------------------------------------
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
        case 0:
            DrawArm(v, c, cx, cy, 1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, -1, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, 1, gap, size, thick, colour);
            break;
        case 1:
            DrawArm(v, c, cx, cy, 1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -1, 0, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, -1, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, 0, 1, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, kDiag, -kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, -kDiag, gap, size, thick, colour);
            break;
        case 2:
            DrawArm(v, c, cx, cy, kDiag, -kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, kDiag, kDiag, gap, size, thick, colour);
            DrawArm(v, c, cx, cy, -kDiag, -kDiag, gap, size, thick, colour);
            break;
        case 3: {
            float s = std::max(thick * 0.5f, 1.0f * unit);
            WriteSegment(v, c, cx, cy - s, cx, cy + s, s * 2, colour);
            break;
        }
        default:
            break;
    }
}

// ---- scope (COD 2 / COD 3 style) ---------------------------------------------------------------------------
//
// Detection: the player must be aiming down sights (zoom > 1) AND have reached at least 70% of the
// weapon's aim zoom, so the scope appears when the gun is mostly up (like COD). In all-weapons mode
// any weapon that reaches this threshold triggers the scope. In sniper-only mode the zoom must also
// be at least 3.0x (sniper-level).
bool IsScoped(const game::Options& opts) {
    if (opts.scope_style == 0) return false;
    if (!R32(kWorld) || !g_camera) return false;
    uint32_t me = R32(kWorld) + kWorldPlayer;
    uint32_t inv = me + kPlayerInventory;
    uint32_t weapon = R32(inv + kInvHeld);
    if (!weapon) return false;
    uint32_t data = R32(weapon + kWeaponData);
    if (!data) return false;
    float aim = G<float>(data + kDataAimZoom);
    if (aim <= 1.0f) return false;
    float zoom = G<float>(me + kPlayerZoom);
    if (zoom < 1.01f) return false;
    // ADS progress: (zoom - 1) / (aim - 1) = 0 hip, 1 full aim
    float progress = (zoom - 1.0f) / (aim - 1.0f);
    if (progress < 0.7f) return false;
    if (opts.scope_all_weapons) return true;
    return zoom >= 3.0f;
}

void DrawScopeSegments(Vertex*& v, uint8_t*& c, float vx, float vy, float vw, float vh, float unit) {
    const game::Options& opts = game::Opt();
    float size_pct = float(std::clamp(opts.scope_size, 10u, 100u)) / 100.0f;
    float diameter = vh * size_pct;
    float radius = diameter * 0.5f;
    float cx = vx + vw * 0.5f;
    float cy = vy + vh * 0.5f;

    // Vignette: black ring from the circle edge to the screen corners.
    float black[4] = {0.0f, 0.0f, 0.0f, float(std::clamp(opts.scope_vignette_opacity, 0u, 100u)) / 100.0f};
    constexpr int kSegments = 32;
    float outer_r = std::sqrt(vw * vw + vh * vh) * 0.5f + 16.0f;
    for (int i = 0; i < kSegments; i++) {
        float a0 = 2.0f * 3.14159265f * float(i) / float(kSegments);
        float a1 = 2.0f * 3.14159265f * float(i + 1) / float(kSegments);
        float cos0 = std::cos(a0), sin0 = std::sin(a0);
        float cos1 = std::cos(a1), sin1 = std::sin(a1);
        float x0i = cx + cos0 * radius;
        float y0i = cy + sin0 * radius;
        float x1i = cx + cos1 * radius;
        float y1i = cy + sin1 * radius;
        float x0o = cx + cos0 * outer_r;
        float y0o = cy + sin0 * outer_r;
        float x1o = cx + cos1 * outer_r;
        float y1o = cy + sin1 * outer_r;
        // Triangle 1: inner0, inner1, outer0
        *v++ = {x0i, y0i}; std::memcpy(c, black, 16); c += 16;
        *v++ = {x1i, y1i}; std::memcpy(c, black, 16); c += 16;
        *v++ = {x0o, y0o}; std::memcpy(c, black, 16); c += 16;
        // Triangle 2: inner1, outer1, outer0
        *v++ = {x1i, y1i}; std::memcpy(c, black, 16); c += 16;
        *v++ = {x1o, y1o}; std::memcpy(c, black, 16); c += 16;
        *v++ = {x0o, y0o}; std::memcpy(c, black, 16); c += 16;
    }

    // Crosshair: shape depends on scope_style.
    float col[4];
    CurrentScopeColour(col);
    float thick = float(std::clamp(opts.scope_thickness, 1u, 10u)) * unit;
    float arm = radius * 0.9f;

    switch (opts.scope_style) {
        case 1:  // COD 2: simple thin cross
            WriteSegment(v, c, cx - arm, cy, cx + arm, cy, thick, col);
            WriteSegment(v, c, cx, cy - arm, cx, cy + arm, thick, col);
            break;
        case 2: {  // COD 3: mil-dot
            WriteSegment(v, c, cx - arm, cy, cx + arm, cy, thick, col);
            WriteSegment(v, c, cx, cy - arm, cx, cy + arm, thick, col);
            float dot_sz = thick * 1.8f;
            for (int i = 1; i <= 4; i++) {
                float d = radius * 0.2f * float(i);
                WriteSegment(v, c, cx + d - dot_sz * 0.5f, cy, cx + d + dot_sz * 0.5f, cy, dot_sz, col);
                WriteSegment(v, c, cx - d - dot_sz * 0.5f, cy, cx - d + dot_sz * 0.5f, cy, dot_sz, col);
                WriteSegment(v, c, cx, cy + d - dot_sz * 0.5f, cx, cy + d + dot_sz * 0.5f, dot_sz, col);
                WriteSegment(v, c, cx, cy - d - dot_sz * 0.5f, cx, cy - d + dot_sz * 0.5f, dot_sz, col);
            }
            break;
        }
        case 3: {  // MODERN: thick cross with center dot
            float thick_m = thick * 1.5f;
            WriteSegment(v, c, cx - arm, cy, cx + arm, cy, thick_m, col);
            WriteSegment(v, c, cx, cy - arm, cx, cy + arm, thick_m, col);
            float dot = thick * 3.0f;
            WriteSegment(v, c, cx - dot * 0.5f, cy, cx + dot * 0.5f, cy, dot, col);
            break;
        }
        case 4:  // PSO-1: cross with rangefinder chevrons below center
            WriteSegment(v, c, cx - arm, cy, cx + arm, cy, thick, col);
            WriteSegment(v, c, cx, cy - arm, cx, cy + arm, thick, col);
            for (int i = 1; i <= 4; i++) {
                float y = cy + radius * 0.15f * float(i);
                float w = thick * (2.0f + float(i));
                WriteSegment(v, c, cx - w, y, cx + w, y, thick, col);
            }
            break;
        default:
            break;
    }
}

// Combined overlay draw (scope + crosshair) in a single triangle batch.
void DrawOverlays(X86Context& ctx, float unit) {
    const game::Options& opts = Opt();
    bool scope_on = IsScoped(opts);
    bool crosshair_on = opts.crosshair && !scope_on;
    if (!scope_on && !crosshair_on) return;

    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
    float cx = vx + vw * 0.5f;
    float cy = vy + vh * 0.5f;

    static const uint32_t buffer = mem::AllocSystem(kCombinedOverlayVerts * sizeof(Vertex), 16);
    static const uint32_t offset = mem::AllocSystem(8, 16);
    static const uint32_t colours = mem::AllocSystem(kCombinedOverlayVerts * sizeof(float) * 4, 16);

    Vertex* first = static_cast<Vertex*>(HostPtr(buffer));
    Vertex* v = first;
    uint8_t* c = static_cast<uint8_t*>(HostPtr(colours));

    if (scope_on) DrawScopeSegments(v, c, vx, vy, vw, vh, unit);
    else if (crosshair_on) DrawCrosshairSegments(v, c, cx, cy, unit);

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
    // Kept as a thin wrapper for backwards compatibility — DrawEsp calls DrawOverlays,
    // which handles both scope and crosshair. Direct calls to DrawCrosshair still work.
    const game::Options& opts = Opt();
    if (!opts.crosshair || !R32(kWorld)) return;
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
    float cx = vx + vw * 0.5f;
    float cy = vy + vh * 0.5f;
    static const uint32_t buffer = mem::AllocSystem(64 * sizeof(Vertex), 16);
    static const uint32_t offset = mem::AllocSystem(8, 16);
    static const uint32_t colours = mem::AllocSystem(64 * sizeof(float) * 4, 16);
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
    DrawOverlays(ctx, unit);  // scope + crosshair (independent of the ESP master toggle)
    if (!opts.esp || !g_camera || !R32(kWorld)) return;
    uint32_t viewport = SYM(D3D_Device) + XDK_CDevice_Viewport;
    float vx = float(R32(viewport)), vy = float(R32(viewport + 4));
    float vw = float(R32(viewport + 8)), vh = float(R32(viewport + 12));
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
