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
//      rate now FOLLOWS THE LEVEL: forced releases via direct sub_001E8830 calls bring
//      additional grenades per cycle, on top of the natural release.
//
//      Rate model:
//        - Natural throw cycle: ~30 frames per throw (2/sec at 60 fps), bounded by the throw
//          animation's release frame.
//        - Forced extra releases: 1 attempt every (30 / level) frames, on top of natural.
//        - Level 2: ~4/sec total. Level 8: ~16/sec. Level 30+: ~62/sec (frame-capped).
//
//      Forced release mechanics:
//        - We call sub_001E8830 (the release event handler) with a synthetic event struct
//          whose first field points to the character.
//        - If the count drops after the call, the release WORKED — a grenade was spawned.
//        - We restore the count (infinite grenades) and continue.
//        - If the count does NOT drop, the release FAILED (animation state wrong) — we
//          permanently disable forcing for this session and fall back to natural rate.
//        - The g_throw_just_fired flag is also set by the hook on each successful forced
//          release, so our natural handler still clears the animation on the next frame.
//
//      If forcing is disabled (game rejects calls mid-animation), you still get continuous
//      natural spam at ~2/sec — same behavior as before.
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
constexpr uint32_t kReleaseEvent = 0x001E8830;  // sub_001E8830: the animation release event
constexpr int kMaxRapidLevel = 8;

// Two independent animation-clear counters.
int g_sniper_counter = kMaxRapidLevel;
int g_explosive_counter = kMaxRapidLevel;

// Set true by sub_001E8830 when the grenade count drops (a throw just fired). Consumed by
// ExtraRapidFire to clear the throw animations right after the release.
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

// ---- forced grenade release (rate scaling) ----------------------------------------------------------
//
// Tries to call sub_001E8830 with a synthetic event struct whose first field is the character.
// Returns true if a grenade was spawned (count dropped), false otherwise.
//
// Safety:
//   - All general-purpose registers are saved and restored around the call.
//   - If the count does NOT drop (release rejected due to animation state), returns false.
//   - The caller disables forcing after kMaxForceFailures consecutive failures.
//
// The synthetic event struct: just a 16-byte buffer whose first uint32 is the character
// pointer. The hook sub_001E8830 reads only R32(who) to get the character, so this minimal
// struct is what it needs. The original function may read more, but if it strictly requires
// animation state to spawn, the count won't drop and we detect the failure.
bool TryForceRelease(X86Context& ctx, uint32_t me) {
    static uint32_t event_buf = 0;
    if (!event_buf) event_buf = mem::AllocSystem(16, 16);
    W32(event_buf, me);       // who->character at offset 0
    W32(event_buf + 4, 0);    // padding
    W32(event_buf + 8, 0);    // padding
    W32(event_buf + 12, 0);   // padding

    uint16_t before = G<uint16_t>(me + kCharGrenades);
    if (before < 1) return false;

    // Save registers the game function may clobber.
    uint32_t save_eax = ctx.eax, save_ecx = ctx.ecx, save_edx = ctx.edx;
    uint32_t save_esi = ctx.esi, save_edi = ctx.edi, save_ebx = ctx.ebx;

    // Call sub_001E8830(?, ?, who). Only arg3 matters — it is at esp+0xC as read by our hook.
    CallGuest(ctx, kReleaseEvent, {0, 0, event_buf});

    ctx.eax = save_eax; ctx.ecx = save_ecx; ctx.edx = save_edx;
    ctx.esi = save_esi; ctx.edi = save_edi; ctx.ebx = save_ebx;

    uint16_t after = G<uint16_t>(me + kCharGrenades);
    if (after < before) {
        // Release worked — restore the count (infinite grenades) and report success.
        G<uint16_t>(me + kCharGrenades) = before;
        return true;
    }
    return false;
}

constexpr int kMaxForceFailures = 5;

// Called from the player's update AFTER the game's own update. Runs three independent jobs:
//  (A) Sniper slow-fire (SNR, SN5): gated by rapid_fire > 1, throttled by rapid_fire.
//  (B) Explosive slow-fire (RPG, GRL, BNS/M203): gated by rapid_fire_extra > 1, throttled by rapid_fire_extra.
//  (C) Hand grenades: natural spam at ~2/sec + forced extra releases scaling with rapid_fire_extra.
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

    // (C) Hand grenades — natural spam + forced releases scaling with rapid_fire_extra.
    if (grenades_on) {
        // Safety net: always keep at least one grenade available so a new throw can start.
        uint16_t& nades = G<uint16_t>(me + kCharGrenades);
        if (nades < 1) nades = 1;

        // Natural release detected — clear animations so the next natural throw can start.
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

        // Forced extra releases scaling with rapid_fire_extra level.
        // Interval = 30 / level frames. At level 2: 15 frames (4/sec). Level 8: 3 frames
        // (20/sec). Level 30+: 1 frame (60/sec). Failures disable forcing permanently.
        static int force_failures = 0;
        static int force_interval_counter = 0;
        static bool force_disabled = false;

        if (!force_disabled && o.rapid_fire_extra > 1) {
            int level = int(o.rapid_fire_extra);
            int interval = std::max(1, 30 / level);
            force_interval_counter++;
            if (force_interval_counter >= interval) {
                force_interval_counter = 0;
                if (TryForceRelease(ctx, me)) {
                    force_failures = 0;  // reset on success
                } else {
                    force_failures++;
                    if (force_failures >= kMaxForceFailures) {
                        force_disabled = true;
                        LOGI("granade: forced release rejected by game state, using natural rate");
                    }
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
