// The cheats page's god mode, one-hit kill, infinite grenades, extra rapid fire and wallbang. The rest live
// where the game already has a hook: infinite ammo, rapid fire and no recoil with the weapon's, game speed
// with the frame time
#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <set>
#include <string>

#include "common/options.h"
#include "core/log.h"          // LOGI for the wallbang diagnostic
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

constexpr uint32_t kDifficulty = 0x2014C;
constexpr float kFullHealth[] = {1200.0f, 750.0f};

// weapon manager + slow-fire detection
constexpr uint32_t kWeaponManager = 0x2D12D0;
constexpr uint32_t kTableCount = 1;
constexpr uint32_t kTableEntries = 4;
constexpr uint32_t kEntrySize = 0x20;
constexpr uint32_t kWeaponType = 0;
constexpr uint32_t kWeaponAnim = 0xD8;
constexpr uint32_t kWeaponData = 0xEC;     // offset from weapon object to its data block pointer
constexpr uint32_t kPlayerInventory = 0x278;
constexpr uint32_t kInvOwner = 0x1C;
constexpr uint32_t kInvSlots = 0x20;       // array of two weapon slot pointers
constexpr uint32_t kInvHeld = 0x24;
constexpr uint32_t kInvSwitchTo = 0x28;
constexpr uint32_t kInvRequest = 0x38;
constexpr uint32_t kInvBusy = 0xB4;
constexpr uint32_t kSuppressorBusy = 0x13;
constexpr int kMaxRapidLevel = 8;
int g_slowfire_counter = kMaxRapidLevel;
std::atomic<bool> g_throw_just_fired{false};

// ---- wallbang -------------------------------------------------------------------------------------
// Every weapon carries a `penetration` float in its data block. Typical values are 0.5-1.0 in COD2-derived
// weapon defs (pistol 0.3, rifle 0.7, sniper 1.0). Raising it to kWallbangValue makes every bullet pass
// through any wall. Damage (20-50), range (500+), clip size, and ammo type live in the same block but at
// different numeric ranges, so a value filter of 0.3..1.5 catches penetration and recoil but skips damage,
// range, clip and ammo.
//
// Offsets scanned: 0x3C..0xB0, step 4, skipping 0x60 and 0x64 (clip size and ammo type — known ints).
// The value stored is kWallbangValue (10.0). On first use per level, the game log shows exactly which
// offsets were modified:
//   wallbang: modified [slot0: 3C=0.55 48=0.70 5C=0.50 | slot1: 48=0.70 |]
// so if a side effect appears (recoil kicks wildly, fire rate slow), the log tells us where to narrow.
constexpr float kWallbangValue = 10.0f;
constexpr uint32_t kPenetrationCandidates[] = {
    0x3C, 0x40, 0x44, 0x48, 0x4C, 0x50, 0x54, 0x58, 0x5C,
    // 0x60 and 0x64 skipped (clip size, ammo type)
    0x68, 0x6C, 0x70, 0x74, 0x78, 0x7C, 0x80, 0x84, 0x88, 0x8C, 0x90,
    0x94, 0x98, 0x9C, 0xA0, 0xA4, 0xA8, 0xAC, 0xB0
};
bool g_wallbang_logged = false;

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

// ---- ApplyWallbang: paksa penetrasi senjata ke nilai maksimum --------------------------------------
void ApplyWallbang() {
    if (!game::Opt().wallbang) return;
    uint32_t me = Player();
    if (!me || R32(me + kCharDying)) return;
    uint32_t inv = me + kPlayerInventory;
    if (R32(inv + kInvOwner) != me) return;
    uint32_t slots = R32(inv + kInvSlots);
    if (!slots) return;
    bool log_now = !g_wallbang_logged;
    char buf[512] = {0};
    int pos = 0;
    for (int i = 0; i < 2; i++) {
        uint32_t weapon = R32(slots + uint32_t(i) * 4);
        if (!weapon) continue;
        uint32_t data = R32(weapon + kWeaponData);
        if (!data) continue;
        if (log_now && pos < int(sizeof(buf)) - 16) {
            pos += std::snprintf(buf + pos, sizeof(buf) - pos, "slot%d:", i);
        }
        for (uint32_t off : kPenetrationCandidates) {
            float cur = G<float>(data + off);
            if (cur > 0.3f && cur < 1.5f) {
                if (log_now && pos < int(sizeof(buf)) - 16) {
                    pos += std::snprintf(buf + pos, sizeof(buf) - pos, " %X=%.2f",
                                         unsigned(off), cur);
                }
                G<float>(data + off) = kWallbangValue;
            }
        }
        if (log_now && pos < int(sizeof(buf)) - 1) {
            pos += std::snprintf(buf + pos, sizeof(buf) - pos, " |");
        }
    }
    if (log_now && pos > 0) {
        LOGI("wallbang: modified [%s]", buf);
        g_wallbang_logged = true;
    }
}

// ---- ExtraRapidFire: continuous slow-fire + throw-event grenade clear ------------------------------
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

// The player's update. God mode runs BEFORE the game's update (health full and invulnerable flag set when
// the game checks damage). Wallbang runs BEFORE the game's update so the game sees max penetration when it
// processes the shot. Extra rapid fire runs AFTER the game's update so its clears are the LAST writes of the
// frame.
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
        ApplyWallbang();   // <-- wallbang: set max penetration on both slots every frame
    }
    __imp__sub_001E5FA0(ctx, base);
    if (is_player) {
        ExtraRapidFire();  // <-- after the game update: our clears are the last writes of the frame
    }
}

// A soldier takes a bullet
X86_EXTERN_FUNC(__imp__sub_001E74B0);
X86_FUNC(sub_001E74B0) {
    Doomed doomed = Doom(ctx.ecx, R32(ctx.esp + 0x18));
    __imp__sub_001E74B0(ctx, base);
    Spare(doomed);
}

// A character's animation event (the throw takes a grenade). We restore the count and set g_throw_just_fired
// so ExtraRapidFire clears the throw animations on this same frame right after the release.
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
