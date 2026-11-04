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
// The four slow fire weapons keep their fire pacing in their own first person animation AND are single
// shot by design (the trigger is edge-triggered for them), so the regular rapid_fire multiplier never
// reaches them. To make them fire continuously while the trigger is held — like the AK-47 — we do two
// things when rapid_fire_extra is on and rapid_fire is any non-OFF level (2X through 8X):
//
//   1. Set the weapon's fire mode to "auto" (1), so a held trigger keeps firing without needing the
//      player to release and re-press it.
//   2. Clear the weapon's kWeaponAnim on a fraction of the frames proportional to rapid_fire, so the
//      game accepts the next shot every 8/rapid_fire frames (at 8X every frame, at 2X every fourth).
//
// The grenade throw is similar: with infinite_grenades also on, a throw starts a recovery window during
// which the animation state is cleared at the same proportional rate, so the throw repeats at the
// rapid_fire level.
constexpr uint32_t kWeaponManager = 0x2D12D0;
constexpr uint32_t kTableCount = 1;      // byte at the table's start
constexpr uint32_t kTableEntries = 4;    // pointer to the entry array
constexpr uint32_t kEntrySize = 0x20;    // name key (8 bytes), weapon data, ...
constexpr uint32_t kWeaponType = 0;      // byte: index in the table
constexpr uint32_t kWeaponAnim = 0xD8;   // the first person animation id, 0 when idle
constexpr uint32_t kWeaponData = 0xEC;   // pointer to the weapon's data block
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

// Fire-mode byte: where in the weapon's data block the single-shot / full-auto flag lives. 0 = single
// shot, 1 = full auto. This offset is a guess based on comparing the RPG's data block to the AK-47's;
// if the weapon still needs per-shot trigger pulls with rapid_fire_extra on, this offset is wrong — try
// +0x14, +0x1C, or +0x20 next, and check the "weapons: auto-fire on weapon" log line to confirm the
// change took effect. Restored to its original value when the option goes off or the weapon leaves hand.
constexpr uint32_t kDataFireMode = 0x18;
uint32_t g_auto_fire_weapon = 0;      // the weapon we converted to auto-fire, 0 when none
uint8_t  g_auto_fire_original = 0;    // its fire-mode byte before we changed it

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

// Restore the fire mode on the weapon we converted to auto, if any. Called when rapid_fire_extra goes
// off, the player dies, the weapon leaves hand, or the level ends.
void RestoreAutoFire() {
    if (!g_auto_fire_weapon) return;
    uint32_t data = R32(g_auto_fire_weapon + kWeaponData);
    if (data) G<uint8_t>(data + kDataFireMode) = g_auto_fire_original;
    g_auto_fire_weapon = 0;
    g_auto_fire_original = 0;
}

// Force full-auto fire mode on the given slow-fire weapon, saving its original byte for the restore.
// Only logs the first time a given weapon is converted so the log stays readable during a play session.
void SetAutoFire(uint32_t weapon) {
    uint32_t data = R32(weapon + kWeaponData);
    if (!data) return;
    if (g_auto_fire_weapon == weapon) {
        // Same weapon, make sure the byte is still 1 (in case something else wrote it)
        if (G<uint8_t>(data + kDataFireMode) != 1) G<uint8_t>(data + kDataFireMode) = 1;
        return;
    }
    // Different weapon: restore the previous one first
    RestoreAutoFire();
    g_auto_fire_weapon = weapon;
    g_auto_fire_original = G<uint8_t>(data + kDataFireMode);
    if (g_auto_fire_original != 1) G<uint8_t>(data + kDataFireMode) = 1;
    // Log so the offset can be verified (RPG has 0 here, AK-47 has 1)
    LOGI("weapons: auto-fire on weapon %08X (data + %X: %u -> 1)", unsigned(weapon),
         unsigned(kDataFireMode), unsigned(g_auto_fire_original));
}

// Called from the player's update each frame. Three jobs, all gated on rapid_fire_extra being on:
//  1. Auto-fire: set the in-hand slow-fire weapon's fire mode to auto so a held trigger keeps firing,
//     restoring it when the weapon leaves hand or the option goes off.
//  2. Slow fire throttle: clear the weapon's animation field on a fraction of the frames proportional to
//     rapid_fire, capping the fire rate at 8/rapid_fire frames between shots (8X every frame, 2X every
//     fourth). Combined with (1), holding the trigger fires continuously at the rapid_fire level.
//  3. Grenades: after a throw starts a short recovery window, the held weapon's animation and the
//     inventory's busy flag are cleared at the same proportional rate, so the throw repeats at the
//     rapid_fire level. Only when infinite_grenades is also on.
// A rapid_fire of 1 (OFF) makes this a no-op and resets all state.
void ExtraRapidFire() {
    const game::Options& o = game::Opt();
    if (!o.rapid_fire_extra || o.rapid_fire <= 1) {
        RestoreAutoFire();
        g_grenade_recovery = 0;
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    uint32_t me = Player();
    if (!me || R32(me + kCharDying)) {
        RestoreAutoFire();
        g_grenade_recovery = 0;
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    uint32_t inv = me + kPlayerInventory;
    if (R32(inv + kInvOwner) != me) {
        RestoreAutoFire();
        g_grenade_recovery = 0;
        g_slowfire_counter = kMaxRapidLevel;
        g_grenade_counter = kMaxRapidLevel;
        return;
    }
    const bool switching = R32(inv + kInvRequest) != 0 || R32(inv + kInvHeld) != R32(inv + kInvSwitchTo);
    uint32_t held = R32(inv + kInvHeld);
    const bool slow_in_hand = !switching && held && SlowFireHeld(held);

    // (1) auto-fire: only for the slow-fire weapon actually in hand and idle; restore otherwise
    if (slow_in_hand) SetAutoFire(held);
    else RestoreAutoFire();

    // (2) slow fire throttle — proportional to rapid_fire level
    const int step = int(o.rapid_fire);
    g_slowfire_counter -= step;
    if (g_slowfire_counter <= 0) {
        g_slowfire_counter = kMaxRapidLevel;
        if (slow_in_hand) {
            if (R32(held + kWeaponAnim)) W32(held + kWeaponAnim, 0);
        }
    }

    // (3) grenade throw recovery — same throttle, separate counter
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
        ExtraRapidFire();  // rapid fire extra: auto-fire + slow fire throttle + grenade throw recovery
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
