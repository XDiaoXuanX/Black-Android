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

// ---- extra rapid fire ------------------------------------------------------------------------------
// RAPID FIRE EXTRA is level-based (1 = OFF, 2..999) and drives two independent things:
//
//  (A) Sniper slow-fire (SNR, SN5) is NOT part of this — those are under RAPID FIRE.
//
//  (B) EXPLOSIVE slow-fire weapons (RPG, GRL, BNS/M203): gated by rapid_fire_extra > 1 only.
//      The held weapon's animation is cleared at a rate proportional to rapid_fire_extra (2X =
//      every 4th frame, 8X+ = every frame). rapid_fire_extra = 1 disables this branch.
//      The M16 primary fire (BNS) is separately handled by rapid_fire's wait divider in hooks.cpp.
//      NOTE: infinite_ammo must be ON so the M203's alt ammo (kWeaponAltAmmo, +0xF8) does not
//      deplete; infinite_grenades is NOT needed (that is for hand grenades only).
//
//  (C) Hand grenades: gated by rapid_fire_extra > 1 AND infinite_grenades ALONE. NO rapid_fire
//      level required: grenades throw continuously, with no pause, no delay, no recovery window,
//      even when RAPID FIRE is OFF. The throw animation lives on the CHARACTER at +0xD8 (there is
//      no grenade weapon object), and it must be allowed to play until the "release" event fires
//      (sub_001E8830 receives it, the grenade count drops). If we cleared the animation before that
//      event, the animation would never reach its release frame and no grenade would come out.
//
//      The fix is to CLEAR ONLY WHEN THE EVENT HAS JUST FIRED. sub_001E8830 sets g_throw_just_fired
//      on every release (count drop). ExtraRapidFire consumes the flag with exchange(false) and
//      clears the character's animation, the held weapon's animation, and the inventory busy flag
//      exactly once per throw:
//        frame N    : player presses throw, animation starts
//        frame N+X  : animation reaches release, sub_001E8830 fires → count drop → g_throw_just_fired = true
//        frame N+X  : ExtraRapidFire (this file) runs right after the game update → clears animation
//        frame N+X+1: game sees animation idle, throw button still held → new throw starts
//        cycle repeats every ~X frames (the animation's own release timing, ~20-30 at 60 fps)
//
//      This gives 2-3 throws per second — frame-rate limited only by the animation's own release
//      timing, not by a fixed recovery window. The throw button only needs to be held; we clear
//      exactly at release so the next throw starts the very next frame: continuous spam with no
//      pause. The rate does NOT scale with rapid_fire_extra level — it is bounded by the throw
//      animation's natural release frame. Scaling it would require advancing the animation's time
//      field by (rate - 1) * dt each frame, which needs the animation controller's time-field
//      offset that this code does not currently know; writing to the wrong offset would corrupt
//      the animation state and crash the game. What follows the rapid_fire_extra level is
//      CONTINUOUS SPAM, independent of the exact level (any level > 1).
constexpr uint32_t kWeaponManager = 0x2D12D0;
constexpr uint32_t kTableCount = 1;
constexpr uint32_t kTableEntries = 4;
constexpr uint32_t kEntrySize = 0x20;
constexpr uint32_t kWeaponType = 0;
constexpr uint32_t kWeaponAnim = 0xD8;
constexpr uint32_t kPlayerInventory = 0x278;
constexpr uint32_t kInvOwner = 0x1C;
constexpr uint32_t kInvHeld = 0x24;
constexpr uint32_t kInvSwitchTo = 0x28;
constexpr uint32_t kInvRequest = 0x38;
constexpr uint32_t kInvBusy = 0xB4;
constexpr uint32_t kSuppressorBusy = 0x13;
constexpr int kMaxRapidLevel = 8;

// Two independent animation-clear counters: one driven by rapid_fire (SNR, SN5), one by
// rapid_fire_extra (RPG, GRL, BNS/M203). Each counts down by the respective level's value and
// triggers a clear when it reaches zero.
int g_sniper_counter = kMaxRapidLevel;
int g_explosive_counter = kMaxRapidLevel;

// Set true by sub_001E8830 when the grenade count drops (a throw just fired); ExtraRapidFire consumes
// it to clear the throw animations right after the release. Using exchange(false) means the clears
// happen exactly once per throw event, not on every frame — which is what lets the animation play
// its natural course up to the release frame before we reset it.
std::atomic<bool> g_throw_just_fired{false};

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

// Which slow-fire category a weapon belongs to.
//   kCatSniper   : SNR, SN5 — driven by rapid_fire
//   kCatExplosive: RPG, GRL, BNS — driven by rapid_fire_extra
//   kCatNone     : everything else
enum SlowCategory { kCatNone = 0, kCatSniper, kCatExplosive };

SlowCategory SlowCategoryOf(uint32_t weapon) {
    static const std::set<std::string> kSnipers = {"SNR", "SN5"};
    static const std::set<std::string> kExplosives = {"RPG", "GRL", "BNS"};
    if (!weapon) return kCatNone;
    uint32_t manager = R32(kWeaponManager);
    uint32_t table = manager ? R32(manager) : 0;
    if (!table) return kCatNone;
    uint32_t entries = R32(table + kTableEntries);
    uint8_t count = G<uint8_t>(table + kTableCount);
    int type = G<uint8_t>(weapon + kWeaponType);
    if (!entries || count == 0 || count > 64 || type >= count) return kCatNone;
    uint32_t e = entries + uint32_t(type) * kEntrySize;
    std::string code = CodeOf(DecodeName(R32(e) | uint64_t(R32(e + 4)) << 32));
    if (kExplosives.count(code)) return kCatExplosive;
    if (kSnipers.count(code)) return kCatSniper;
    return kCatNone;
}

// Called from the player's update AFTER the game's own update (see sub_001E5FA0 below). Runs three
// INDEPENDENT jobs:
//  (A) Sniper slow-fire (SNR, SN5): gated by rapid_fire > 1, throttled by rapid_fire.
//  (B) Explosive slow-fire (RPG, GRL, BNS/M203): gated by rapid_fire_extra > 1, throttled by rapid_fire_extra.
//  (C) Hand grenades: gated by rapid_fire_extra > 1 + infinite_grenades ALONE — no rapid_fire level required.
//      Continuous spam, no pause, no delay, no recovery window. Works even when RAPID FIRE is OFF.
void ExtraRapidFire() {
    const game::Options& o = game::Opt();
    bool sniper_on = o.rapid_fire > 1;
    bool explosive_on = o.rapid_fire_extra > 1;
    bool grenades_on = o.rapid_fire_extra > 1 && o.infinite_grenades;

    if (!sniper_on && !explosive_on && !grenades_on) {
        g_throw_just_fired = false;
        g_sniper_counter = kMaxRapidLevel;
        g_explosive_counter = kMaxRapidLevel;
        return;
    }
    uint32_t me = Player();
    if (!me || R32(me + kCharDying)) {
        g_throw_just_fired = false;
        g_sniper_counter = kMaxRapidLevel;
        g_explosive_counter = kMaxRapidLevel;
        return;
    }
    uint32_t inv = me + kPlayerInventory;
    if (R32(inv + kInvOwner) != me) {
        g_throw_just_fired = false;
        g_sniper_counter = kMaxRapidLevel;
        g_explosive_counter = kMaxRapidLevel;
        return;
    }

    // Read the weapon in hand once: its category decides which counter path applies. If a weapon
    // switch is in progress, cat stays kCatNone and neither counter triggers a clear this frame.
    uint32_t weapon = 0;
    SlowCategory cat = kCatNone;
    if (!R32(inv + kInvRequest) && R32(inv + kInvHeld) == R32(inv + kInvSwitchTo)) {
        weapon = R32(inv + kInvHeld);
        cat = SlowCategoryOf(weapon);
    }

    // (A) Sniper slow-fire (SNR, SN5) — throttled by rapid_fire only.
    if (sniper_on) {
        g_sniper_counter -= int(o.rapid_fire);
        if (g_sniper_counter <= 0) {
            g_sniper_counter = kMaxRapidLevel;
            if (cat == kCatSniper && weapon) W32(weapon + kWeaponAnim, 0);
        }
    } else {
        g_sniper_counter = kMaxRapidLevel;
    }

    // (B) Explosive slow-fire (RPG, GRL, BNS/M203) — throttled by rapid_fire_extra only.
    if (explosive_on) {
        g_explosive_counter -= int(o.rapid_fire_extra);
        if (g_explosive_counter <= 0) {
            g_explosive_counter = kMaxRapidLevel;
            if (cat == kCatExplosive && weapon) W32(weapon + kWeaponAnim, 0);
        }
    } else {
        g_explosive_counter = kMaxRapidLevel;
    }

    // (C) Hand grenades — continuous throwing, independent of rapid_fire level. Each time a release
    // fires (sub_001E8830 sees the count drop and sets g_throw_just_fired), clear the throw
    // animation, the weapon animation and the busy flag. The game then sees the character idle with
    // the throw button still held and starts the next throw the next frame — no pause, no delay,
    // no recovery window. The animation value check (< 0x1000) prevents clearing a pointer by mistake.
    if (grenades_on) {
        // Safety net: keep at least one grenade available so the next throw can start even if the
        // game somehow drained the count without sub_001E8830 seeing it (defensive, normally redundant).
        uint16_t& nades = G<uint16_t>(me + kCharGrenades);
        if (nades < 1) nades = 1;

        if (g_throw_just_fired.exchange(false)) {
            uint32_t held = R32(inv + kInvHeld);
            if (held) {
                uint32_t hv = R32(held + kWeaponAnim);
                if (hv < 0x1000) W32(held + kWeaponAnim, 0);
            }
            uint32_t mv = R32(me + kWeaponAnim);
            if (mv < 0x1000) W32(me + kWeaponAnim, 0);
            uint32_t busy = R32(inv + kInvBusy);
            if (busy && busy != kSuppressorBusy) W32(inv + kInvBusy, 0);
        }
    } else {
        g_throw_just_fired = false;
    }
}

}  // namespace

// The player's update. God mode runs BEFORE the game's update so health is full and the invulnerable flag is
// set when the game checks damage. Extra rapid fire runs AFTER the game's update so its clears are the LAST
// writes of the frame and the game starts the next frame with animation = 0 (idle).
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
        ExtraRapidFire();  // AFTER the game update: our clears are the last writes of the frame
    }
}

// A soldier takes a bullet
X86_EXTERN_FUNC(__imp__sub_001E74B0);
X86_FUNC(sub_001E74B0) {
    Doomed doomed = Doom(ctx.ecx, R32(ctx.esp + 0x18));
    __imp__sub_001E74B0(ctx, base);
    Spare(doomed);
}

// A character's animation event (the throw takes a grenade). With infinite_grenades on, we restore the count
// and set g_throw_just_fired so ExtraRapidFire clears the throw animations on this same frame — right after
// the release — so the next frame can start a fresh throw. The animation is NOT cleared here directly: this
// hook runs INSIDE the game's player update, and clearing mid-update would be overwritten by the game's own
// animation state machine (sub_0017B130 in hooks.cpp). Doing it in ExtraRapidFire after the update makes the
// clear the last write of the frame and lets the game start the next frame with animation = 0.
//
// The g_throw_just_fired flag is set ONLY when rapid_fire_extra > 1 — hand grenades are part of
// rapid_fire_extra and do NOT require a rapid_fire level (RAPID FIRE may be OFF and grenades still spam).
X86_EXTERN_FUNC(__imp__sub_001E8830);
X86_FUNC(sub_001E8830) {
    uint32_t who = R32(ctx.esp + 0xC);
    uint32_t me = who ? R32(who) : 0;
    bool keep = game::Opt().infinite_grenades && me && me == Player();
    uint16_t grenades = keep ? G<uint16_t>(me + kCharGrenades) : 0;
    __imp__sub_001E8830(ctx, base);
    if (keep && G<uint16_t>(me + kCharGrenades) < grenades) {
        G<uint16_t>(me + kCharGrenades) = grenades;
        // Hand grenades are part of rapid_fire_extra: only rapid_fire_extra > 1 gates them, no rapid_fire
        // level needed.
        if (game::Opt().rapid_fire_extra > 1) g_throw_just_fired = true;
    }
}
