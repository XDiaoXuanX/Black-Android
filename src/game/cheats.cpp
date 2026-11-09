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
//      The held weapon's animation is cleared at a rate proportional to rapid_fire_extra.
//
//  (C) Hand grenades: gated by rapid_fire_extra > 1 AND infinite_grenades ALONE. The throw
//      rate is now FRAME-RATE LIMITED to match RPG fire rate:
//        - Every frame, we attempt a FORCED RELEASE via a direct call to sub_001E8830 (the
//          animation release handler) with a synthetic event struct.
//        - If the call spawns a grenade (count drops), we restore the count (infinite
//          grenades) and let the animation clear from our hook. Success = 1 grenade/frame
//          = 60 throws/sec at 60 fps — the SAME rate as a max-level RPG.
//        - If the call is rejected by the game (count does NOT drop because animation state
//          does not permit release), we count a failure. After 5 consecutive failures we
//          permanently disable forcing and fall back to the natural animation-driven rate
//          (~2-3 throws/sec — still continuous spam, but not frame-rate).
//
//      The synthetic event struct passed to sub_001E8830 is 16 bytes with the character
//      pointer at offset 0 (which is what our hook reads at esp+0xC → R32(who)). All
//      general-purpose registers are saved and restored around the CallGuest so the game's
//      own update loop is not disturbed.
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
constexpr uint32_t kReleaseEvent = 0x001E8830;  // sub_001E8830: the throw's release handler
constexpr int kMaxRapidLevel = 8;

int g_sniper_counter = kMaxRapidLevel;
int g_explosive_counter = kMaxRapidLevel;

// Set true by sub_001E8830 (the real hook) when a grenade count drops. ExtraRapidFire consumes
// it to clear the throw animations right after the release.
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

// Clear the throw-related animation fields so the game sees "idle" next frame.
void ClearThrowAnimations(uint32_t me, uint32_t inv) {
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

// Try to force a grenade release by calling sub_001E8830 with a synthetic event struct.
// Returns true if the grenade count dropped (a grenade spawned), false otherwise.
//
// Safety:
//   - All general-purpose registers (eax, ecx, edx, esi, edi, ebx) are saved and restored
//     around the CallGuest so the caller's state is preserved.
//   - The event struct is 16 bytes, first field = character pointer (what the hook reads).
//   - If the game rejects the call (count does not drop), we detect it and return false.
//   - The caller disables forcing after 5 consecutive failures.
bool TryForceRelease(X86Context& ctx, uint32_t me) {
    static uint32_t event_buf = 0;
    if (!event_buf) event_buf = mem::AllocSystem(16, 16);
    W32(event_buf, me);
    W32(event_buf + 4, 0);
    W32(event_buf + 8, 0);
    W32(event_buf + 12, 0);

    uint16_t before = G<uint16_t>(me + kCharGrenades);
    if (before < 1) return false;

    uint32_t save_eax = ctx.eax, save_ecx = ctx.ecx, save_edx = ctx.edx;
    uint32_t save_esi = ctx.esi, save_edi = ctx.edi, save_ebx = ctx.ebx;

    // sub_001E8830(arg1, arg2, who) — the hook only reads who at esp+0xC.
    CallGuest(ctx, kReleaseEvent, {0, 0, event_buf});

    ctx.eax = save_eax; ctx.ecx = save_ecx; ctx.edx = save_edx;
    ctx.esi = save_esi; ctx.edi = save_edi; ctx.ebx = save_ebx;

    uint16_t after = G<uint16_t>(me + kCharGrenades);
    if (after < before) {
        // Success — restore count (infinite grenades) and report.
        G<uint16_t>(me + kCharGrenades) = before;
        return true;
    }
    return false;
}

constexpr int kMaxForceFailures = 5;

// Called from the player's update AFTER the game's own update. Runs three independent jobs:
//  (A) Sniper slow-fire (SNR, SN5): throttled by rapid_fire.
//  (B) Explosive slow-fire (RPG, GRL, BNS/M203): throttled by rapid_fire_extra.
//  (C) Hand grenades: natural animation rate + FORCED frame-rate release (1/frame = 60/sec,
//      matching RPG at max rapid_fire_extra). Falls back to natural if forcing is rejected.
void ExtraRapidFire(X86Context& ctx) {
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

    uint32_t weapon = 0;
    SlowCategory cat = kCatNone;
    if (!R32(inv + kInvRequest) && R32(inv + kInvHeld) == R32(inv + kInvSwitchTo)) {
        weapon = R32(inv + kInvHeld);
        cat = SlowCategoryOf(weapon);
    }

    // (A) Sniper slow-fire — throttled by rapid_fire only.
    if (sniper_on) {
        g_sniper_counter -= int(o.rapid_fire);
        if (g_sniper_counter <= 0) {
            g_sniper_counter = kMaxRapidLevel;
            if (cat == kCatSniper && weapon) W32(weapon + kWeaponAnim, 0);
        }
    } else {
        g_sniper_counter = kMaxRapidLevel;
    }

    // (B) Explosive slow-fire — throttled by rapid_fire_extra only.
    if (explosive_on) {
        g_explosive_counter -= int(o.rapid_fire_extra);
        if (g_explosive_counter <= 0) {
            g_explosive_counter = kMaxRapidLevel;
            if (cat == kCatExplosive && weapon) W32(weapon + kWeaponAnim, 0);
        }
    } else {
        g_explosive_counter = kMaxRapidLevel;
    }

    // (C) Hand grenades — frame-rate forced release to match RPG fire rate.
    if (grenades_on) {
        // Safety net: keep at least one grenade so a new throw can start.
        uint16_t& nades = G<uint16_t>(me + kCharGrenades);
        if (nades < 1) nades = 1;

        // Natural animation release — clear so next natural throw can start.
        if (g_throw_just_fired.exchange(false)) {
            ClearThrowAnimations(me, inv);
        }

        // Forced frame-rate releases: one attempt per frame. If successful → 60 throws/sec
        // (same as RPG at max level). If rejected by game state 5x in a row → disable and
        // fall back to natural (~2/sec). The user still gets continuous spam either way.
        static int force_failures = 0;
        static bool force_disabled = false;

        if (!force_disabled) {
            if (TryForceRelease(ctx, me)) {
                force_failures = 0;
                // Clear animations so the next natural cycle can also start.
                ClearThrowAnimations(me, inv);
            } else {
                force_failures++;
                if (force_failures >= kMaxForceFailures) {
                    force_disabled = true;
                    LOGI("grenade: forced release rejected by game, using natural rate");
                }
            }
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
        ExtraRapidFire(ctx);  // AFTER the game update: our clears are the last writes of the frame
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
// the release — so the next frame can start a fresh throw.
X86_EXTERN_FUNC(__imp__sub_001E8830);
X86_FUNC(sub_001E8830) {
    uint32_t who = R32(ctx.esp + 0xC);
    uint32_t me = who ? R32(who) : 0;
    bool keep = game::Opt().infinite_grenades && me && me == Player();
    uint16_t grenades = keep ? G<uint16_t>(me + kCharGrenades) : 0;
    __imp__sub_001E8830(ctx, base);
    if (keep && G<uint16_t>(me + kCharGrenades) < grenades) {
        G<uint16_t>(me + kCharGrenades) = grenades;
        if (game::Opt().rapid_fire_extra > 1) g_throw_just_fired = true;
    }
}
