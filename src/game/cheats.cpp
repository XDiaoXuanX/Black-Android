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
// Slow-fire weapons (RPG, GRL, Walther 2000 = SNR/SN5) keep the throttle-based clear: the held weapon's
// animation is cleared at a fraction of frames proportional to rapid_fire (2X = every 4th frame, 8X = every
// frame). This is the version from the recovery-window revert and works reliably.
//
// Grenades are different. The throw animation lives on the CHARACTER at +0xD8 (there is no grenade weapon
// object), and it must be allowed to play until the "release" event fires (sub_001E8830 receives it, the
// grenade count drops). If we cleared the animation before that event, the animation would never reach its
// release frame and no grenade would come out. If we cleared after a fixed recovery window, the window might
// be shorter than the animation's natural release frame and the same "no grenade" problem would occur.
//
// The fix here is to CLEAR ONLY WHEN THE EVENT HAS JUST FIRED. sub_001E8830 sets g_throw_just_fired on every
// release (count drop). ExtraRapidFire consumes the flag with exchange(false) and clears the character's
// animation, the held weapon's animation, and the inventory busy flag exactly once per throw. The result is:
//   frame N   : player presses throw, animation starts
//   frame N+X : animation reaches release, sub_001E8830 fires → count drop → g_throw_just_fired = true
//   frame N+X : ExtraRapidFire runs (after update) → clears animation → flag back to false
//   frame N+X+1: game sees animation idle, throw button still held → new throw starts
//   cycle repeats every ~X frames, where X is the animation's natural release frame (~20-30 at 60 fps)
// This gives 2-3 throws per second (frame-rate limited only by the animation's own release timing, not by a
// fixed recovery window). If it turns out the game is edge-triggered (needs a release + re-press between
// throws), the animation clear would allow it but each throw would still need the player to tap the button;
// in that case the log will show only one throw despite holding the button and we will need to find a
// different approach (a diagnostic logging pass to locate the throw request field).
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

// Called from the player's update AFTER the game's own update (see sub_001E5FA0 below). Runs two jobs:
//  (1) Slow fire weapons: throttled by rapid_fire level (unchanged from the recovery-window version).
//  (2) Grenades: clear the throw animation and the busy flag exactly once per throw event, immediately
//      after the event fires. This is the minimum work needed to allow the next throw without breaking
//      the animation that must complete to actually release the grenade.
void ExtraRapidFire() {
    const game::Options& o = game::Opt();
    if (!o.rapid_fire_extra || o.rapid_fire <= 1) {
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

    // (1) Slow-fire weapons — throttled by rapid_fire level, unchanged
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

    // (2) Grenade — clear exactly once per throw event (right after the release). The safety check on the
    // animation value prevents clearing anything larger than 0x1000, which would be a pointer rather than
    // an animation id (would crash the game if we wrote 0 to it).
    if (o.infinite_grenades && g_throw_just_fired.exchange(false)) {
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
X86_EXTERN_FUNC(__imp__sub_001E8830);
X86_FUNC(sub_001E8830) {
    uint32_t who = R32(ctx.esp + 0xC);
    uint32_t me = who ? R32(who) : 0;
    bool keep = game::Opt().infinite_grenades && me && me == Player();
    uint16_t grenades = keep ? G<uint16_t>(me + kCharGrenades) : 0;
    __imp__sub_001E8830(ctx, base);
    if (keep && G<uint16_t>(me + kCharGrenades) < grenades) {
        G<uint16_t>(me + kCharGrenades) = grenades;
        if (game::Opt().rapid_fire_extra) g_throw_just_fired = true;
    }
}
