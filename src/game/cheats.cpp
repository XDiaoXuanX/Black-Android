// The cheats page's god mode, one-hit kill, infinite grenades and extra rapid fire. The rest live where the
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
// rapid_fire multiplier never reaches them. With rapid_fire_extra on and rapid_fire at any non-OFF level
// (2X through 8X), the in-hand slow fire weapon's kWeaponAnim is cleared on a fraction of the frames
// proportional to the rapid_fire level: at 8X (the panel's maximum) every frame, at 4X every other frame,
// at 2X every fourth.
//
// Grenades are kept throwable AT ALL TIMES while rapid_fire_extra + infinite_grenades are both on — there
// is no recovery window any more. The held weapon's animation, the inventory's busy flag, AND the
// character's own animation field (same offset as kWeaponAnim, which is where the throw cooldown usually
// lives on COD characters) are all cleared at the same proportional rate. This gives continuous grenade
// throwing that mirrors how RPG/GRL keep firing while the trigger is held.
constexpr uint32_t kWeaponManager = 0x2D12D0;
constexpr uint32_t kTableCount = 1;      // byte at the table's start
constexpr uint32_t kTableEntries = 4;    // pointer to the entry array
constexpr uint32_t kEntrySize = 0x20;    // name key (8 bytes), weapon data, ...
constexpr uint32_t kWeaponType = 0;      // byte: index in the table
constexpr uint32_t kWeaponAnim = 0xD8;   // the first person animation id, 0 when idle. On characters this
                                          // offset usually stores the throw/action animation cooldown too.
constexpr uint32_t kPlayerInventory = 0x278;
constexpr uint32_t kInvOwner = 0x1C;
constexpr uint32_t kInvHeld = 0x24;      // the weapon in hand
constexpr uint32_t kInvSwitchTo = 0x28;  // the weapon a switch brings out
constexpr uint32_t kInvRequest = 0x38;   // 2 asks to switch to the other slot, as the d-pad does
// the inventory's busy flag: 0x13 while the suppressor animation runs (weapons.cpp kInvBusy). Cleared
// during a grenade throw so the next throw can start, but never when it holds 0x13 (that is the
// suppressor's, which weapons.cpp waits on).
constexpr uint32_t kInvBusy = 0xB4;
constexpr uint32_t kSuppressorBusy = 0x13;
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
//  2. Grenades: the game's throw cooldown is defeated by clearing (a) the held weapon's animation,
//     (b) the inventory's busy flag, and (c) the character's own animation field (usually where the
//     throw cooldown lives), at the same proportional rate. This keeps the throw action always ready
//     while infinite_grenades is on — continuous throwing without ever running out or waiting.
// A rapid_fire of 1 (OFF) makes this a no-op and resets the throttle counters.
void ExtraRapidFire() {
    const game::Options& o = game::Opt();
    if (!o.rapid_fire_extra || o.rapid_fire <= 1) {
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    uint32_t me = Player();
    if (!me || R32(me + kCharDying)) {
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    uint32_t inv = me + kPlayerInventory;
    if (R32(inv + kInvOwner) != me) {
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

    // (2) grenade throw — kept throwable AT ALL TIMES while infinite_grenades is on, no recovery
    // window. Same throttle as slow-fire weapons. Clears the held weapon's animation, the
    // inventory's busy flag, AND the character's own animation field (which is where the throw
    // cooldown usually lives on COD characters — same offset as kWeaponAnim by convention).
    if (!o.infinite_grenades) {
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    g_grenade_counter -= step;
    if (g_grenade_counter <= 0) {
        g_grenade_counter = kMaxRapidLevel;
        // (a) held weapon's animation (safe even when the held slot is empty)
        uint32_t held = R32(inv + kInvHeld);
        if (held && R32(held + kWeaponAnim)) W32(held + kWeaponAnim, 0);
        // (b) inventory's busy flag, but never the suppressor's 0x13 (weapons.cpp waits on that)
        uint32_t busy = R32(inv + kInvBusy);
        if (busy && busy != kSuppressorBusy) W32(inv + kInvBusy, 0);
        // (c) character's own animation field — the throw animation lives on the character, not
        // on any weapon, so the cooldown that gates the next throw is likely at this same offset
        uint32_t charAnim = R32(me + kWeaponAnim);
        if (charAnim) W32(me + kWeaponAnim, 0);
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
        ExtraRapidFire();  // rapid fire extra: slow fire weapons + continuous grenade throwing
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
// (0x1E8934), the cheat puts the player's back so the count never drops. The throw recovery used to be
// triggered here too — it is not any more: the grenade section of ExtraRapidFire now runs continuously
// while infinite_grenades is on, so there is no recovery window to set.
X86_EXTERN_FUNC(__imp__sub_001E8830);
X86_FUNC(sub_001E8830) {
    uint32_t who = R32(ctx.esp + 0xC);
    uint32_t me = who ? R32(who) : 0;
    bool keep = game::Opt().infinite_grenades && me && me == Player();
    uint16_t grenades = keep ? G<uint16_t>(me + kCharGrenades) : 0;
    __imp__sub_001E8830(ctx, base);
    if (keep && G<uint16_t>(me + kCharGrenades) < grenades) {
        G<uint16_t>(me + kCharGrenades) = grenades;
    }
}
