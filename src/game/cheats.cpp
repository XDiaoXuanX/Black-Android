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
constexpr uint32_t kCharInvulnerable = 0x8F2;
constexpr uint32_t kPlayerKind = 2;

constexpr uint32_t kDifficulty = 0x2014C;  // in the game object, 0 easy to 3 black ops
constexpr float kFullHealth[] = {1200.0f, 750.0f};

// ---- extra rapid fire: every weapon that has its pacing in an animation, not in the wait field ----------
// The wait-between-shots field at +0xDC is handled by the regular rapid_fire cheat in hooks.cpp
// (sub_001EEFD0). This extra cheat is for weapons whose NEXT shot is gated by an ANIMATION instead — mostly
// the launchers and pump/bolt weapons. Clearing their animation every frame (after the game's own update,
// see the caller) makes the next shot available immediately, so they fire at frame rate just like the
// rapid_fire weapons do.
//
// The slow-fire set is all weapons that keep their own pacing in the first person animation:
//   RPG  = rocket launcher
//   GRL  = grenade launcher
//   SNR  = Walther 2000 (bolt-action sniper)
//   SN5  = Walther 2000 silenced
//   BNS  = M16A2 + M203 (the underbarrel launcher has its own animation pacing)
//   SHG  = SPAS 12 (pump-action: the pump animation gates the next shot)
//   SH5  = SPAS 12 silenced/silver
//   RM1  = Remington 870 (pump-action)
//   RM5  = Remington 870 silenced/silver
//   MGN  = Magnum (revolver cylinder animation)
//   MG5  = Magnum silenced/silver
// Everything else (AK-47, M4, SMG, etc.) uses the regular rapid_fire multiplier and is handled by the
// hooks.cpp wait hook; with rapid_fire = INFINITE (999) they now fire at frame rate too because hooks.cpp
// no longer clamps to 8X.
constexpr uint32_t kWeaponManager = 0x2D12D0;
constexpr uint32_t kTableCount = 1;
constexpr uint32_t kTableEntries = 4;
constexpr uint32_t kEntrySize = 0x20;
constexpr uint32_t kWeaponType = 0;
constexpr uint32_t kWeaponAnim = 0xD8;   // first person animation id / weapon state. 0 when idle.
constexpr uint32_t kPlayerInventory = 0x278;
constexpr uint32_t kInvOwner = 0x1C;
constexpr uint32_t kInvHeld = 0x24;
constexpr uint32_t kInvSwitchTo = 0x28;
constexpr uint32_t kInvRequest = 0x38;
constexpr uint32_t kInvBusy = 0xB4;
constexpr uint32_t kSuppressorBusy = 0x13;

uint32_t Player() { return R32(kWorld) ? R32(kWorld) + kWorldPlayer : 0; }

float FullHealth() { return kFullHealth[R32(R32(kGame) + kDifficulty) >= 2]; }

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

bool g_invulnerable;

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

std::string CodeOf(const std::string& name) {
    return name.size() > 4 && name.compare(0, 4, "BG1_") == 0 ? name.substr(4) : name;
}

// every weapon that gates its next shot on an animation rather than on the wait field at +0xDC. Adding a
// weapon here means its animation is cleared every frame while rapid_fire_extra is on, so the game accepts
// the next trigger pull immediately — continuous fire, no pause, no delay.
bool SlowFireHeld(uint32_t weapon) {
    static const std::set<std::string> kSlowCodes = {
        "RPG", "GRL", "SNR", "SN5",  // original four: launchers + bolt-action sniper
        "BNS",                       // M16A2 + M203 underbarrel launcher
        "SHG", "SH5",                // SPAS 12 pump-action shotgun
        "RM1", "RM5",                // Remington 870 pump-action shotgun
        "MGN", "MG5",                // Magnum revolver
    };
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

// Called from the player's update AFTER the game's own update (see the caller below). Clears every
// animation / busy gate at once, every frame, no throttle, no counter, no recovery window.
//
// CRITICAL ORDERING: this runs after __imp__sub_001E5FA0(ctx, base), so our writes are the LAST ones in the
// frame and the animation stays 0 until the next frame's game update starts. If we cleared BEFORE the
// game's update, the update would immediately set the animation back and the next shot would be rejected.
void ExtraRapidFire() {
    const game::Options& o = game::Opt();
    if (!o.rapid_fire_extra || o.rapid_fire <= 1) return;
    uint32_t me = Player();
    if (!me || R32(me + kCharDying)) return;
    uint32_t inv = me + kPlayerInventory;
    if (R32(inv + kInvOwner) != me) return;

    // (1) Slow-fire weapons: clear the held weapon's fire animation every frame so the game accepts the
    // next trigger pull immediately. Unconditional write so it stays 0 even if the game set it earlier.
    if (!R32(inv + kInvRequest) && R32(inv + kInvHeld) == R32(inv + kInvSwitchTo)) {
        uint32_t weapon = R32(inv + kInvHeld);
        if (weapon && SlowFireHeld(weapon)) {
            W32(weapon + kWeaponAnim, 0);
        }
    }

    // (2) Inventory busy flag: clear every frame regardless of grenade state (this gates BOTH firing and
    // throwing on some weapons). Never touch the suppressor's 0x13 (weapons.cpp waits on that).
    uint32_t busy = R32(inv + kInvBusy);
    if (busy && busy != kSuppressorBusy) W32(inv + kInvBusy, 0);

    // (3) Grenade: the throw animation lives on the character (no grenade weapon object), so clear the
    // character's own kWeaponAnim every frame while infinite_grenades is on.
    if (o.infinite_grenades) {
        uint32_t held = R32(inv + kInvHeld);
        if (held) W32(held + kWeaponAnim, 0);
        W32(me + kWeaponAnim, 0);
    }
}

}  // namespace

// The player's update. God mode runs BEFORE the game's update (health is full and the invulnerable flag is
// set when the game checks damage). Extra rapid fire runs AFTER the game's update (our cooldown clears are
// the LAST writes of the frame and the animation stays 0 for the whole next frame).
X86_EXTERN_FUNC(__imp__sub_001E5FA0);
X86_FUNC(sub_001E5FA0) {
    uint32_t me = ctx.ecx;
    bool is_player = me && me == Player() && R32(me + kCharKind) == kPlayerKind;
    if (is_player) {
        if (game::Opt().god_mode && !R32(me + kCharDying)) {
            float& health = G<float>(me + kCharHealth);
            health = std::max(health, FullHealth());
            G<uint8_t>(me + kCharInvulnerable) = 1;
            g_invulnerable = true;
        } else if (g_invulnerable) {
            G<uint8_t>(me + kCharInvulnerable) = 0;
            g_invulnerable = false;
        }
    }
    __imp__sub_001E5FA0(ctx, base);
    if (is_player) {
        ExtraRapidFire();  // <-- AFTER the game update: our clear is the last write of the frame
    }
}

// A soldier takes a bullet
X86_EXTERN_FUNC(__imp__sub_001E74B0);
X86_FUNC(sub_001E74B0) {
    Doomed doomed = Doom(ctx.ecx, R32(ctx.esp + 0x18));
    __imp__sub_001E74B0(ctx, base);
    Spare(doomed);
}

// A character's animation event: the throw takes a grenade, the cheat puts the player's back so the count
// never drops. The throw recovery is not triggered here any more — ExtraRapidFire keeps every cooldown
// cleared continuously so there is no recovery window to set.
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
