// The cheats page's god mode, one-hit kill, infinite grenades, extra rapid fire and fly mode.
// The rest live where the game already has a hook: infinite ammo, rapid fire and no recoil with the
// weapon's, game speed with the frame time
#include <algorithm>
#include <atomic>// The cheats page's god mode, one-hit kill, infinite grenades and extra rapid fire. The rest live where the
// game already has a hook: infinite ammo, rapid fire and no recoil with the weapon's, game speed with the
// frame time
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include "common/options.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {

// character: the player is the one in the world container, its kind 2
constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharGrenades = 0x286;  // uint16
constexpr uint32_t kCharHealth = 0x2F0;    // float, the difficulty's full at most
constexpr uint32_t kCharDying = 0x384;     // not 0 once it is killed
// byte the scripts set: the player's damage calls (its class's 0x1ED990, 0x1ED700, 0x1ED2B0, 0x1ED5D0, and
// 0x1ED1E0) take nothing while it is
constexpr uint32_t kCharInvulnerable = 0x8F2;
constexpr uint32_t kPlayerKind = 2;

constexpr uint32_t kDifficulty = 0x2014C;  // in the game object, 0 easy to 3 black ops
// full health, as 0x13F10 scales 1.0: easy and normal, then hard and black ops
constexpr float kFullHealth[] = {1200.0f, 750.0f};

// ---- extra rapid fire: RPG, GRL, Walther 2000 (SNR/SN5) and grenades ---------------------------------
// The four slow fire weapons keep their fire pacing in their own first person animation, so the regular
// rapid_fire multiplier never reaches them. The grenade throw has its own recovery animation played after
// the throw event. With rapid_fire_extra on and rapid_fire at any non-OFF level (2X through 8X), the
// in-hand slow fire weapon's kWeaponAnim is cleared on a fraction of the frames proportional to the
// rapid_fire level: at 8X (the panel's maximum) every frame, at 4X every other frame, at 2X every fourth.
// With infinite_grenades also on, a throw starts a recovery window during which the animation state is
// cleared at the same proportional rate, so both the slow-fire weapons and the grenade throw end up
// firing at the same multiplier rapid_fire gives the other guns.
constexpr uint32_t kWeaponManager = 0x2D12D0;
constexpr uint32_t kTableCount = 1;      // byte at the table's start
constexpr uint32_t kTableEntries = 4;    // pointer to the entry array
constexpr uint32_t kEntrySize = 0x20;    // name key (8 bytes), weapon data, ...
constexpr uint32_t kWeaponType = 0;      // byte: index in the table
constexpr uint32_t kWeaponAnim = 0xD8;   // the first person animation id, 0 when idle
constexpr uint32_t kPlayerInventory = 0x278;
constexpr uint32_t kInvOwner = 0x1C;
constexpr uint32_t kInvHeld = 0x24;      // the weapon in hand
constexpr uint32_t kInvSwitchTo = 0x28;  // the weapon a switch brings out
constexpr uint32_t kInvRequest = 0x38;   // 2 asks to switch to the other slot, as the d-pad does
// the inventory's busy flag: 0x13 while the suppressor animation runs (weapons.cpp kInvBusy). Cleared
// during a grenade's throw recovery so the next throw can start, but never when it holds 0x13 (that is
// the suppressor's, which weapons.cpp waits on).
constexpr uint32_t kInvBusy = 0xB4;
constexpr uint32_t kSuppressorBusy = 0x13;
// frames the throw recovery is kept cleared. 20 frames is about a third of a second at 60 fps — enough
// to swallow the throw's animation tail without touching a later unrelated animation.
constexpr int kGrenadeRecoveryFrames = 20;
std::atomic<int> g_grenade_recovery{0};
// The panel's RAPID FIRE steps from 1 to 8. kMaxRapidLevel is the counter's reset value, decremented by
// rapid_fire each frame: at 8 it hits 0 every frame (clear every frame), at 4 every other frame, at 2
// every fourth. The two counters move together but are kept separate so the slow-fire weapon and the
// grenade do not steal each other's throttle slot.
constexpr int kMaxRapidLevel = 8;
int g_slowfire_counter = kMaxRapidLevel;
int g_grenade_counter = kMaxRapidLevel;

uint32_t Player() { return R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0; }

float FullHealth() { return kFullHealth[R32(R32(kGame) + kDifficulty) >= 2]; }

// One-hit kill: a hit of the player's on another character starts from a sliver of health, any damage kills.
// A hit that took nothing gives the health back
constexpr float kSliver = 0.001f;

struct Doomed {
    uint32_t who = 0;
    float health = 0;
};

Doomed Doom(uint32_t victim, uint32_t attacker) {
    uint32_t me = Player();
    if (!game::Opt().one_hit_kill || !me || attacker != me || victim == me || R32(victim + kCharDying)) return {};
    float& health = G<float>(victim + kCharHealth);
    if (!(health > kSliver)) return {};
    Doomed kept{victim, health};
    health = kSliver;
    return kept;
}

void Spare(const Doomed& kept) {
    if (!kept.who || R32(kept.who + kCharDying)) return;
    float& health = G<float>(kept.who + kCharHealth);
    if (health > 0) health = kept.health - (kSliver - health);
}

bool g_invulnerable;  // the flag is ours, cleared when god mode goes off

// ---- extra rapid fire helpers ------------------------------------------------------------------------

// twelve characters of " -/0-9A-Z_" in base 40, as the game keeps the name keys
std::string DecodeName(uint64_t key) {
    char s[13] = {};
    for (int i = 11; i >= 0; i--) {
        int c = int(key % 40);
        key /= 40;
        s[i] = c == 0 ? ' ' : c == 1 ? '-' : c == 2 ? '/' : c < 13 ? char('0' + c - 3)
              : c < 39 ? char('A' + c - 13) : '_';
    }
    std::string out = s;
    out.erase(out.find_last_not_of(' ') + 1);
    return out;
}

// strip the optional BG1_ the game puts before some codes (as weapons.cpp CodeOf does)
std::string CodeOf(const std::string& name) {
    return name.size() > 4 && name.compare(0, 4, "BG1_") == 0 ? name.substr(4) : name;
}

// true when the weapon's type is one of the four slow fire ones
bool SlowFireHeld(uint32_t weapon) {
    static const std::set<std::string> kSlowCodes = {"RPG", "GRL", "SNR", "SN5"};
    uint32_t manager = R32(kWeaponManager);
    uint32_t table = manager ? R32(manager) : 0;
    if (!table) return false;
    uint32_t entries = R32(table + kTableEntries);
    uint8_t count = G<uint8_t>(table + kTableCount);
    int type = G<uint8_t>(weapon + kWeaponType);
    if (!entries || count == 0 || count > 64 || type >= count) return false;
    uint32_t e = entries + uint32_t(type) * kEntrySize;
    std::string code = CodeOf(DecodeName(R32(e) | uint64_t(R32(e + 4)) << 32));
    return kSlowCodes.count(code) != 0;
}

// Called from the player's update each frame. Two jobs, both throttled proportionally to rapid_fire:
//  1. Slow fire weapons (RPG, GRL, Walther 2000): the weapon's animation field is cleared on a fraction
//     of frames proportional to the rapid_fire level, so the weapon fires at the same multiplier the
//     regular rapid_fire gives the other guns. Only when a slow fire weapon is in hand and idle.
//  2. Grenades: after a throw starts a short recovery window, the held weapon's animation and the
//     inventory's busy flag are cleared at the same proportional rate, so the throw repeats at the
//     rapid_fire level. Only when rapid_fire_extra + infinite_grenades are both on.
// A rapid_fire of 1 (OFF) makes this a no-op and resets the throttle counters.
void ExtraRapidFire() {
    const game::Options& o = game::Opt();
    if (!o.rapid_fire_extra || o.rapid_fire <= 1) {
        g_grenade_recovery = 0;
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    uint32_t me = Player();
    if (!me || R32(me + kCharDying)) {
        g_grenade_recovery = 0;
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    uint32_t inv = me + kPlayerInventory;
    if (R32(inv + kInvOwner) != me) {
        g_grenade_recovery = 0;
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    const int step = int(o.rapid_fire);

    // (1) slow fire weapons — throttle by rapid_fire level
    g_slowfire_counter -= step;
    if (g_slowfire_counter <= 0) {
        g_slowfire_counter = kMaxRapidLevel;
        if (!R32(inv + kInvRequest) && R32(inv + kInvHeld) == R32(inv + kInvSwitchTo)) {
            uint32_t weapon = R32(inv + kInvHeld);
            if (weapon && SlowFireHeld(weapon)) {
                if (R32(weapon + kWeaponAnim)) W32(weapon + kWeaponAnim, 0);
            }
        }
    }

    // (2) grenade throw recovery — same throttle, separate counter
    int frames = g_grenade_recovery.load(std::memory_order_relaxed);
    if (frames <= 0) {
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    if (!o.infinite_grenades) {
        g_grenade_recovery = 0;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    g_grenade_recovery = frames - 1;
    g_grenade_counter -= step;
    if (g_grenade_counter <= 0) {
        g_grenade_counter = kMaxRapidLevel;
        uint32_t held = R32(inv + kInvHeld);
        if (held && R32(held + kWeaponAnim)) W32(held + kWeaponAnim, 0);
        uint32_t busy = R32(inv + kInvBusy);
        if (busy && busy != kSuppressorBusy) W32(inv + kInvBusy, 0);
    }
}

}  // namespace

// The player's update, a frame (ecx the player, its class's own): god mode fills the health and holds the flag
// its damage calls test, extra rapid fire keeps a slow fire weapon idle and clears a grenade's throw recovery
X86_EXTERN_FUNC(__imp__sub_001E5FA0);
X86_FUNC(sub_001E5FA0) {
    uint32_t me = ctx.ecx;
    if (me && me == Player() && R32(me + kCharKind) == kPlayerKind) {
        if (game::Opt().god_mode && !R32(me + kCharDying)) {
            float& health = G<float>(me + kCharHealth);
            health = std::max(health, FullHealth());
            G<uint8_t>(me + kCharInvulnerable) = 1;
            g_invulnerable = true;
        } else if (g_invulnerable) {
            G<uint8_t>(me + kCharInvulnerable) = 0;
            g_invulnerable = false;
        }
        ExtraRapidFire();  // rapid fire extra: slow fire weapons + grenade throw recovery
    }
    __imp__sub_001E5FA0(ctx, base);
}

// A soldier takes a bullet (ecx the victim, the sixth of seven arguments its attacker, ret 0x1C): the
// characters' class, the player's has its own
X86_EXTERN_FUNC(__imp__sub_001E74B0);
X86_FUNC(sub_001E74B0) {
    Doomed doomed = Doom(ctx.ecx, R32(ctx.esp + 0x18));
    __imp__sub_001E74B0(ctx, base);
    Spare(doomed);
}

// A character's animation event (64-bit hash, then a pointer to the character): the throw takes a grenade
// (0x1E8934), the cheat puts the player's back. With rapid_fire_extra on, a throw also starts a short
// recovery window during which ExtraRapidFire clears the animation state at the rapid_fire's level so
// the next grenade can be thrown at the same multiplier.
X86_EXTERN_FUNC(__imp__sub_001E8830);
X86_FUNC(sub_001E8830) {
    uint32_t who = R32(ctx.esp + 0xC);
    uint32_t me = who ? R32(who) : 0;
    bool keep = game::Opt().infinite_grenades && me && me == Player();
    uint16_t grenades = keep ? G<uint16_t>(me + kCharGrenades) : 0;
    __imp__sub_001E8830(ctx, base);
    if (keep && G<uint16_t>(me + kCharGrenades) < grenades) {
        G<uint16_t>(me + kCharGrenades) = grenades;
        // the count dropped: a throw just fired. With rapid_fire_extra on, start the recovery window.
        if (game::Opt().rapid_fire_extra) g_grenade_recovery = kGrenadeRecoveryFrames;
    }
}
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include "common/options.h"
#include "game/layout.h"
#include "xdk/hle.h"

using namespace kernel;
using namespace layout;

namespace {

// character: the player is the one in the world container, its kind 2
constexpr uint32_t kCharKind = 0xC4;
constexpr uint32_t kCharGrenades = 0x286;  // uint16
constexpr uint32_t kCharHealth = 0x2F0;    // float, the difficulty's full at most
constexpr uint32_t kCharDying = 0x384;     // not 0 once it is killed
// byte the scripts set: the player's damage calls (its class's 0x1ED990, 0x1ED700, 0x1ED2B0, 0x1ED5D0, and
// 0x1ED1E0) take nothing while it is
constexpr uint32_t kCharInvulnerable = 0x8F2;
constexpr uint32_t kPlayerKind = 2;

constexpr uint32_t kDifficulty = 0x2014C;  // in the game object, 0 easy to 3 black ops
// full health, as 0x13F10 scales 1.0: easy and normal, then hard and black ops
constexpr float kFullHealth[] = {1200.0f, 750.0f};

// ---- extra rapid fire: RPG, GRL, Walther 2000 (SNR/SN5) and grenades ---------------------------------
// The four slow fire weapons keep their fire pacing in their own first person animation, so the regular
// rapid_fire multiplier never reaches them. The grenade throw has its own recovery animation played after
// the throw event. With rapid_fire_extra on and rapid_fire at any non-OFF level (2X through 8X), the
// in-hand slow fire weapon's kWeaponAnim is cleared on a fraction of the frames proportional to the
// rapid_fire level: at 8X (the panel's maximum) every frame, at 4X every other frame, at 2X every fourth.
// With infinite_grenades also on, a throw starts a recovery window during which the animation state is
// cleared at the same proportional rate, so both the slow-fire weapons and the grenade throw end up
// firing at the same multiplier rapid_fire gives the other guns.
constexpr uint32_t kWeaponManager = 0x2D12D0;
constexpr uint32_t kTableCount = 1;      // byte at the table's start
constexpr uint32_t kTableEntries = 4;    // pointer to the entry array
constexpr uint32_t kEntrySize = 0x20;    // name key (8 bytes), weapon data, ...
constexpr uint32_t kWeaponType = 0;      // byte: index in the table
constexpr uint32_t kWeaponAnim = 0xD8;   // the first person animation id, 0 when idle
constexpr uint32_t kPlayerInventory = 0x278;
constexpr uint32_t kInvOwner = 0x1C;
constexpr uint32_t kInvHeld = 0x24;      // the weapon in hand
constexpr uint32_t kInvSwitchTo = 0x28;  // the weapon a switch brings out
constexpr uint32_t kInvRequest = 0x38;   // 2 asks to switch to the other slot, as the d-pad does
// the inventory's busy flag: 0x13 while the suppressor animation runs (weapons.cpp kInvBusy). Cleared
// during a grenade's throw recovery so the next throw can start, but never when it holds 0x13 (that is
// the suppressor's, which weapons.cpp waits on).
constexpr uint32_t kInvBusy = 0xB4;
constexpr uint32_t kSuppressorBusy = 0x13;
// frames the throw recovery is kept cleared. 20 frames is about a third of a second at 60 fps — enough
// to swallow the throw's animation tail without touching a later unrelated animation.
constexpr int kGrenadeRecoveryFrames = 20;
std::atomic<int> g_grenade_recovery{0};
// The panel's RAPID FIRE steps from 1 to 8. kMaxRapidLevel is the counter's reset value, decremented by
// rapid_fire each frame: at 8 it hits 0 every frame (clear every frame), at 4 every other frame, at 2
// every fourth. The two counters move together but are kept separate so the slow-fire weapon and the
// grenade do not steal each other's throttle slot.
constexpr int kMaxRapidLevel = 8;
int g_slowfire_counter = kMaxRapidLevel;
int g_grenade_counter = kMaxRapidLevel;

// ---- fly mode -----------------------------------------------------------------------------------
// The player rises while fly_mode is on, falls back to ground when it goes off. The two offset blocks
// below are GUESSED — the game's character struct has only been reverse-engineered down to +0x8F2
// (invulnerable flag). Position and velocity offsets are not confirmed.
//
// On first enable, the code dumps the guessed values to the log:
//     cheats: fly dump pos=(x,y,z) vel=(x,y,z)
//
// How to interpret the dump:
//   - If pos values look like real coordinates (small numbers, change as you walk): offsets are OK.
//     The player will rise. Done.
//   - If pos values are all zero: the offset is wrong; try kFlyCharPosX = 0x04, 0x08, 0x30, 0x38, 0x40.
//   - If pos values are huge (like 1e+30): wrong offset; try the alternatives above.
//   - If the player moves sideways instead of up: the vertical axis is Y, not Z. Change kFlyCharVelZ
//     to point at kFlyCharVelY (0x20).
//
// Confirmed offsets for reference (from weapons.cpp):
//   +0xC4  kind (uint8)                = 2 for the player
//   +0x286 grenades (uint16)           = what the HUD shows
//   +0x2F0 health (float)              = 1200 (easy/normal) or 750 (hard/black ops)
//   +0x384 dying (byte)                = 0 when alive
//   +0x8F2 invulnerable (byte)         = ours, god mode writes it
constexpr uint32_t kFlyCharPosX = 0x10;  // vec3 (x, y, z) at +0x10, +0x14, +0x18 — GUESSED
constexpr uint32_t kFlyCharPosY = 0x14;
constexpr uint32_t kFlyCharPosZ = 0x18;
constexpr uint32_t kFlyCharVelX = 0x1C;  // vec3 velocity at +0x1C, +0x20, +0x24 — GUESSED
constexpr uint32_t kFlyCharVelY = 0x20;
constexpr uint32_t kFlyCharVelZ = 0x24;
constexpr float kFlyBaseRate = 6.0f;  // units per second at 100% fly_speed
bool g_fly_dumped = false;

uint32_t Player() { return R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0; }

float FullHealth() { return kFullHealth[R32(R32(kGame) + kDifficulty) >= 2]; }

// One-hit kill: a hit of the player's on another character starts from a sliver of health, any damage kills.
// A hit that took nothing gives the health back
constexpr float kSliver = 0.001f;

struct Doomed {
    uint32_t who = 0;
    float health = 0;
};

Doomed Doom(uint32_t victim, uint32_t attacker) {
    uint32_t me = Player();
    if (!game::Opt().one_hit_kill || !me || attacker != me || victim == me || R32(victim + kCharDying)) return {};
    float& health = G<float>(victim + kCharHealth);
    if (!(health > kSliver)) return {};
    Doomed kept{victim, health};
    health = kSliver;
    return kept;
}

void Spare(const Doomed& kept) {
    if (!kept.who || R32(kept.who + kCharDying)) return;
    float& health = G<float>(kept.who + kCharHealth);
    if (health > 0) health = kept.health - (kSliver - health);
}

bool g_invulnerable;  // the flag is ours, cleared when god mode goes off

// ---- extra rapid fire helpers ------------------------------------------------------------------------

// twelve characters of " -/0-9A-Z_" in base 40, as the game keeps the name keys
std::string DecodeName(uint64_t key) {
    char s[13] = {};
    for (int i = 11; i >= 0; i--) {
        int c = int(key % 40);
        key /= 40;
        s[i] = c == 0 ? ' ' : c == 1 ? '-' : c == 2 ? '/' : c < 13 ? char('0' + c - 3)
              : c < 39 ? char('A' + c - 13) : '_';
    }
    std::string out = s;
    out.erase(out.find_last_not_of(' ') + 1);
    return out;
}

// strip the optional BG1_ the game puts before some codes (as weapons.cpp CodeOf does)
std::string CodeOf(const std::string& name) {
    return name.size() > 4 && name.compare(0, 4, "BG1_") == 0 ? name.substr(4) : name;
}

// true when the weapon's type is one of the four slow fire ones
bool SlowFireHeld(uint32_t weapon) {
    static const std::set<std::string> kSlowCodes = {"RPG", "GRL", "SNR", "SN5"};
    uint32_t manager = R32(kWeaponManager);
    uint32_t table = manager ? R32(manager) : 0;
    if (!table) return false;
    uint32_t entries = R32(table + kTableEntries);
    uint8_t count = G<uint8_t>(table + kTableCount);
    int type = G<uint8_t>(weapon + kWeaponType);
    if (!entries || count == 0 || count > 64 || type >= count) return false;
    uint32_t e = entries + uint32_t(type) * kEntrySize;
    std::string code = CodeOf(DecodeName(R32(e) | uint64_t(R32(e + 4)) << 32));
    return kSlowCodes.count(code) != 0;
}

// Called from the player's update each frame. Two jobs, both throttled proportionally to rapid_fire:
//  1. Slow fire weapons (RPG, GRL, Walther 2000): the weapon's animation field is cleared on a fraction
//     of frames proportional to the rapid_fire level, so the weapon fires at the same multiplier the
//     regular rapid_fire gives the other guns. Only when a slow fire weapon is in hand and idle.
//  2. Grenades: after a throw starts a short recovery window, the held weapon's animation and the
//     inventory's busy flag are cleared at the same proportional rate, so the throw repeats at the
//     rapid_fire level. Only when rapid_fire_extra + infinite_grenades are both on.
// A rapid_fire of 1 (OFF) makes this a no-op and resets the throttle counters.
void ExtraRapidFire() {
    const game::Options& o = game::Opt();
    if (!o.rapid_fire_extra || o.rapid_fire <= 1) {
        g_grenade_recovery = 0;
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    uint32_t me = Player();
    if (!me || R32(me + kCharDying)) {
        g_grenade_recovery = 0;
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    uint32_t inv = me + kPlayerInventory;
    if (R32(inv + kInvOwner) != me) {
        g_grenade_recovery = 0;
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    const int step = int(o.rapid_fire);

    // (1) slow fire weapons — throttle by rapid_fire level
    g_slowfire_counter -= step;
    if (g_slowfire_counter <= 0) {
        g_slowfire_counter = kMaxRapidLevel;
        if (!R32(inv + kInvRequest) && R32(inv + kInvHeld) == R32(inv + kInvSwitchTo)) {
            uint32_t weapon = R32(inv + kInvHeld);
            if (weapon && SlowFireHeld(weapon)) {
                if (R32(weapon + kWeaponAnim)) W32(weapon + kWeaponAnim, 0);
            }
        }
    }

    // (2) grenade throw recovery — same throttle, separate counter
    int frames = g_grenade_recovery.load(std::memory_order_relaxed);
    if (frames <= 0) {
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    if (!o.infinite_grenades) {
        g_grenade_recovery = 0;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    g_grenade_recovery = frames - 1;
    g_grenade_counter -= step;
    if (g_grenade_counter <= 0) {
        g_grenade_counter = kMaxRapidLevel;
        uint32_t held = R32(inv + kInvHeld);
        if (held && R32(held + kWeaponAnim)) W32(held + kWeaponAnim, 0);
        uint32_t busy = R32(inv + kInvBusy);
        if (busy && busy != kSuppressorBusy) W32(inv + kInvBusy, 0);
    }
}

// ---- fly mode implementation --------------------------------------------------------------------

// Diagnostic dump once per session (first time fly mode is enabled with a live player). Logged as a
// single line so the user can report it back and we identify the correct offsets.
void FlyDiagnosticOnce(uint32_t me) {
    if (g_fly_dumped) return;
    g_fly_dumped = true;
    float px = G<float>(me + kFlyCharPosX);
    float py = G<float>(me + kFlyCharPosY);
    float pz = G<float>(me + kFlyCharPosZ);
    float vx = G<float>(me + kFlyCharVelX);
    float vy = G<float>(me + kFlyCharVelY);
    float vz = G<float>(me + kFlyCharVelZ);
    LOGI("cheats: fly dump pos=(%.2f,%.2f,%.2f) vel=(%.2f,%.2f,%.2f)", px, py, pz, vx, vy, vz);
    LOGI("cheats: pos should look like player coords; if zeros or huge, adjust kFlyCharPosX/VelX");
}

// Rise the player: overwrite the vertical velocity each frame so the game's physics moves them up.
// Turning fly mode off lets gravity pull them back to ground. Uses the guessed offsets above.
void ApplyFly(uint32_t me) {
    if (!game::Opt().fly_mode) return;
    if (!me || R32(me + kCharDying)) return;
    FlyDiagnosticOnce(me);
    // Sanity guard: if the current velocity value looks absurd, the offset is probably wrong. Skipping
    // the write avoids corrupting unrelated memory (which would crash).
    float cur = G<float>(me + kFlyCharVelZ);
    if (std::abs(cur) > 10000.0f) return;
    float rate = kFlyBaseRate * (game::Opt().fly_speed / 100.0f);
    G<float>(me + kFlyCharVelZ) = rate;
}

}  // namespace

// The player's update, a frame (ecx the player, its class's own): god mode fills the health and holds the flag
// its damage calls test, extra rapid fire keeps a slow fire weapon idle and clears a grenade's throw recovery,
// fly mode rises the player while the option is on
X86_EXTERN_FUNC(__imp__sub_001E5FA0);
X86_FUNC(sub_001E5FA0) {
    uint32_t me = ctx.ecx;
    if (me && me == Player() && R32(me + kCharKind) == kPlayerKind) {
        if (game::Opt().god_mode && !R32(me + kCharDying)) {
            float& health = G<float>(me + kCharHealth);
            health = std::max(health, FullHealth());
            G<uint8_t>(me + kCharInvulnerable) = 1;
            g_invulnerable = true;
        } else if (g_invulnerable) {
            G<uint8_t>(me + kCharInvulnerable) = 0;
            g_invulnerable = false;
        }
        ExtraRapidFire();   // rapid fire extra: slow fire weapons + grenade throw recovery
        ApplyFly(me);       // fly mode: rise while the option is on
    }
    __imp__sub_001E5FA0(ctx, base);
}

// A soldier takes a bullet (ecx the victim, the sixth of seven arguments its attacker, ret 0x1C): the
// characters' class, the player's has its own
X86_EXTERN_FUNC(__imp__sub_001E74B0);
X86_FUNC(sub_001E74B0) {
    Doomed doomed = Doom(ctx.ecx, R32(ctx.esp + 0x18));
    __imp__sub_001E74B0(ctx, base);
    Spare(doomed);
}

// A character's animation event (64-bit hash, then a pointer to the character): the throw takes a grenade
// (0x1E8934), the cheat puts the player's back. With rapid_fire_extra on, a throw also starts a short
// recovery window during which ExtraRapidFire clears the animation state at the rapid_fire's level so
// the next grenade can be thrown at the same multiplier.
X86_EXTERN_FUNC(__imp__sub_001E8830);
X86_FUNC(sub_001E8830) {
    uint32_t who = R32(ctx.esp + 0xC);
    uint32_t me = who ? R32(who) : 0;
    bool keep = game::Opt().infinite_grenades && me && me == Player();
    uint16_t grenades = keep ? G<uint16_t>(me + kCharGrenades) : 0;
    __imp__sub_001E8830(ctx, base);
    if (keep && G<uint16_t>(me + kCharGrenades) < grenades) {
        G<uint16_t>(me + kCharGrenades) = grenades;
        // the count dropped: a throw just fired. With rapid_fire_extra on, start the recovery window.
        if (game::Opt().rapid_fire_extra) g_grenade_recovery = kGrenadeRecoveryFrames;
    }
}
