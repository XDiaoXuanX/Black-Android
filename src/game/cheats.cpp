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
// RAPID FIRE EXTRA drives two independent things:
//
//  (1) Slow-fire weapons — gated by rapid_fire_extra AND rapid_fire > 1. The held weapon's animation is
//      cleared at a fraction of frames proportional to rapid_fire (2X = every 4th frame, 8X = every frame,
//      INFINITE = every frame), so the game accepts the next trigger pull sooner. RAPID FIRE OFF disables
//      this branch: these weapons need a rate multiplier to speed up.
//
//      The list:
//        - RPG   (RPG, single shot, slow reload)
//        - GRL   (Grenade launcher, single shot, slow reload)
//        - SNR   (Walther 2000, bolt action, slow cycle)
//        - SN5   (Walther 2000 silenced, same)
//        - BNS   (M16A2 + M203): the M16 primary fire is already fast via rapid_fire (wait divider in
//                 hooks.cpp sub_001EEFD0). The M203 UNDER-BARREL GRENADE LAUNCHER (alternate fire) is the
//                 slow part: it plays a break-open / load / close animation that gates the next shot.
//                 Clearing kWeaponAnim lets the M203 fire continuously, without pause, without delay.
//                 NOTE: infinite_ammo must be ON so the M203's alt ammo (kWeaponAltAmmo, +0xF8) does not
//                 deplete; infinite_grenades is NOT needed (that is for hand grenades only).
//
//  (2) Grenades (hand grenades) — gated by rapid_fire_extra AND infinite_grenades ALONE. NO rapid_fire level
//      required: grenades throw continuously, with no pause, no delay, no recovery window, even when RAPID
//      FIRE is OFF. The throw animation lives on the CHARACTER at +0xD8 (there is no grenade weapon object),
//      and it must be allowed to play until the "release" event fires (sub_001E8830 receives it, the grenade
//      count drops). If we cleared the animation before that event, the animation would never reach its
//      release frame and no grenade would come out.
//
//      The fix is to CLEAR ONLY WHEN THE EVENT HAS JUST FIRED. sub_001E8830 sets g_throw_just_fired on every
//      release (count drop). ExtraRapidFire consumes the flag with exchange(false) and clears the character's
//      animation, the held weapon's animation, and the inventory busy flag exactly once per throw:
//        frame N    : player presses throw, animation starts
//        frame N+X  : animation reaches release, sub_001E8830 fires → count drop → g_throw_just_fired = true
//        frame N+X  : ExtraRapidFire (this function) runs right after the game update → clears animation
//        frame N+X+1: game sees animation idle, throw button still held → new throw starts
//        cycle repeats every ~X frames (the animation's own release timing, ~20-30 at 60 fps)
//
//      This gives 2-3 throws per second — frame-rate limited only by the animation's own release timing, not
//      by a fixed recovery window. The throw button only needs to be held; we clear exactly at release so the
//      next throw starts the very next frame: continuous spam with no pause.
//
//      NOTE ON RATE: the grenade throw rate does NOT scale with rapid_fire level — it is bounded by the throw
//      animation's natural release frame. Scaling it would require advancing the animation's time field by
//      (rapid_fire - 1) * dt each frame, which needs the animation controller's time-field offset that this
//      code does not currently know; writing to the wrong offset would corrupt the animation state and crash
//      the game. What follows the rapid_fire_extra toggle is CONTINUOUS SPAM, independent of rapid_fire level.
//
//      If the game turns out to be edge-triggered (needs a release + re-press between throws), each throw
//      would still need the player to tap the button; the log will show only one throw despite holding the
//      button and we will need a diagnostic pass to find the throw request field.
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
int g_slowfire_counter = kMaxRapidLevel;
// Set true by sub_001E8830 when the grenade count drops (a throw just fired); ExtraRapidFire consumes it
// to clear the throw animations right after the release. Using exchange(false) means the clears happen
// exactly once per throw event, not on every frame — which is what lets the animation play its natural
// course up to the release frame before we reset it.
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

bool SlowFireHeld(uint32_t weapon) {
    // Slow-fire weapons whose fire animation gates the next shot and needs to be cleared for rapid_fire_extra:
    //   RPG, GRL   single-shot launchers with slow reload
    //   SNR, SN5   Walther 2000 bolt action (silenced / unsilenced)
    //   BNS        M16A2 + M203: the M203 under-barrel launcher's break-open / load / close animation. The
    //              M16 primary fire is already handled by rapid_fire's wait divider, but the M203 alternate
    //              fire is a single-shot launcher with a slow cycle — clearing its animation lets it fire
    //              continuously, without pause, without delay. Requires infinite_ammo (M203 alt ammo at
    //              +0xF8) so the launcher does not run dry.
    static const std::set<std::string> kSlowCodes = {"RPG", "GRL", "SNR", "SN5", "BNS"};
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

// Called from the player's update AFTER the game's own update (see sub_001E5FA0 below). Runs two INDEPENDENT
// jobs:
//  (1) Slow-fire weapons (RPG, GRL, SNR, SN5, BNS): gated by rapid_fire_extra + rapid_fire > 1, throttled by
//      the rapid_fire level. RAPID FIRE OFF disables this branch. For BNS, this clears the M203 under-barrel
//      launcher's slow animation so it fires continuously.
//  (2) Grenades (hand): gated by rapid_fire_extra + infinite_grenades ALONE — no rapid_fire level required.
//      Continuous spam, no pause, no delay, no recovery window. Works even when RAPID FIRE is OFF.
void ExtraRapidFire() {
    const game::Options& o = game::Opt();

    // (1) slow-fire path (incl. BNS M203) needs rapid_fire > 1;
    // (2) hand grenade path needs only the toggle + infinite_grenades.
    bool slowfire_on = o.rapid_fire_extra && o.rapid_fire > 1;
    bool grenades_on = o.rapid_fire_extra && o.infinite_grenades;

    if (!slowfire_on && !grenades_on) {
        g_throw_just_fired = false;
        g_slowfire_counter = kMaxRapidLevel;
        return;
    }
    uint32_t me = Player();
    if (!me || R32(me + kCharDying)) {
        g_throw_just_fired = false;
        g_slowfire_counter = kMaxRapidLevel;
        return;
    }
    uint32_t inv = me + kPlayerInventory;
    if (R32(inv + kInvOwner) != me) {
        g_throw_just_fired = false;
        g_slowfire_counter = kMaxRapidLevel;
        return;
    }

    // (1) Slow-fire weapons — throttled by the rapid_fire level, only when slowfire_on.
    //     Includes BNS: clearing the M203 launcher's animation lets it fire continuously.
    if (slowfire_on) {
        g_slowfire_counter -= int(o.rapid_fire);
        if (g_slowfire_counter <= 0) {
            g_slowfire_counter = kMaxRapidLevel;
            if (!R32(inv + kInvRequest) && R32(inv + kInvHeld) == R32(inv + kInvSwitchTo)) {
                uint32_t weapon = R32(inv + kInvHeld);
                if (weapon && SlowFireHeld(weapon)) {
                    W32(weapon + kWeaponAnim, 0);
                }
            }
        }
    } else {
        g_slowfire_counter = kMaxRapidLevel;
    }

    // (2) Hand grenades — continuous throwing, independent of rapid_fire level. Each time a release fires
    // (sub_001E8830 sees the count drop and sets g_throw_just_fired), clear the throw animation, the
    // weapon animation and the busy flag. The game then sees the character idle with the throw button
    // still held and starts the next throw the next frame — no pause, no delay, no recovery window.
    // The animation value check (< 0x1000) prevents clearing a pointer by mistake.
    if (grenades_on) {
        // Safety net: keep at least one grenade available so the next throw can start even if the game
        // somehow drained the count without sub_001E8830 seeing it (defensive, normally redundant).
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
// The g_throw_just_fired flag is set ONLY when rapid_fire_extra is on — hand grenades are part of
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
        // Hand grenades are part of rapid_fire_extra: only the toggle gates them, no rapid_fire level needed.
        if (game::Opt().rapid_fire_extra) g_throw_just_fired = true;
    }
}
