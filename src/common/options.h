#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <string>

// the game's own settings, in the settings file through app::kGame.options
namespace game {

// the languages, saved by these codes: English, French, German, Italian, Spanish, Indonesian. Indonesian is the
// recomp's own text only: the game files have none, so the game itself stays in English
inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp", "ind"};

struct Options {
    bool skip_intros = false;  // logos at boot and the film after pressing start
    bool fps_counter = false;
    uint32_t fmv_scale = 0;
    uint32_t screen_filter = 0;
    bool reload_blur = true;
    bool bloom = true;
    bool fog = true;
    uint32_t anti_aliasing = 0;
    bool hdr = false;
    uint32_t msaa = 0;
    uint32_t draw_distance = 1;
    bool dynamic_resolution = false;
    uint32_t dynamic_target_fps = 60;
    uint32_t dynamic_min_percent = 40;
    uint32_t dynamic_max_percent = 100;
    uint32_t dynamic_hold = 5;
    float mouse_sensitivity = 1.0f;
    float touch_sensitivity = 1.0f;
    bool invert_mouse = false;
    bool hold_aim = true;
    uint32_t fov = 70;
    uint32_t viewmodel_fov = 70;
    uint32_t walk_speed = 50;
    uint32_t left_deadzone = 0;
    uint32_t right_deadzone = 0;
    float left_sensitivity = 1.0f;
    float right_sensitivity = 1.0f;
    uint32_t audio_output = 0;
    std::string language = "int";
    bool infinite_ammo = false;
    // cheat: times the player's rate of fire, 1 the game's. Applies ONLY to non-explosive weapons
    // (pistols, SMGs, rifles, shotguns, LMGs, and bolt-action snipers SNR/SN5). Explosive weapons
    // (RPG, GRL, BNS/M203) use rapid_fire_extra instead.
    uint32_t rapid_fire = 1;
    // cheat: rapid fire extra — a level like rapid_fire (1 = OFF, 2..999 = level).
    // Only for EXPLOSIVE weapons and hand grenades:
    //   - RPG, GRL (single-shot launchers)
    //   - BNS (M16A2 + M203): the M203 under-barrel launcher's slow cycle
    //   - Hand grenades (throw continuously at any level > 1)
    // For explosives: the wait between shots is divided by rapid_fire_extra, AND the fire animation is
    // cleared at a rate proportional to rapid_fire_extra. For hand grenades: at ANY level > 1, grenades
    // throw continuously, no pause, no delay, no recovery window (bounded by the throw animation's
    // natural release frame, ~2-3 throws/sec at 60 fps).
    uint32_t rapid_fire_extra = 1;
    bool no_recoil = false;
    bool no_bullet_spread = false;
    uint32_t game_speed = 100;
    bool god_mode = false;
    bool one_hit_kill = false;
    bool infinite_grenades = false;
    bool infinite_medkits = false;
    bool unlock_levels = false;
    bool unlock_black_ops = false;
    bool esp = false;
    uint32_t esp_display = 3;
    // cheat: show the enemy's distance from the player, as a small 3-digit number above the
    // health bar (or above the head when the health bar is off). The number is the 3D distance
    // in metres, capped at 999. Independent of the esp_display line/health bits.
    bool esp_distance = false;
    // cheat: directional arrows pointing at enemies. Two modes:
    //   - EDGE:  off-screen enemies get a triangle at the screen edge, pointing toward them.
    //   - ABOVE: on-screen enemies get a small downward triangle just above the head.
    // Mode 0 = EDGE only, 1 = ABOVE only, 2 = BOTH. Size scales the triangle (1..10, pixels at 480p).
    bool esp_arrow = false;
    uint32_t esp_arrow_mode = 2;  // 0 = EDGE, 1 = ABOVE, 2 = BOTH
    uint32_t esp_arrow_size = 4;  // 1..10, pixels at 480p
    uint32_t esp_color = 0;
    uint32_t esp_red = 224;
    uint32_t esp_green = 219;
    uint32_t esp_blue = 209;
    bool crosshair = false;
    uint32_t crosshair_style = 0;
    uint32_t crosshair_size = 8;
    uint32_t crosshair_thickness = 2;
    uint32_t crosshair_gap = 4;
    uint32_t crosshair_color = 0;
    uint32_t crosshair_red = 224;
    uint32_t crosshair_green = 219;
    uint32_t crosshair_blue = 209;
    bool aimbot = false;
    uint32_t aimbot_target = 0;
    uint32_t aimbot_pull = 15;
    uint32_t aimbot_deadzone = 15;
    uint32_t cheat_panel_size = 100;
    uint32_t cheat_button_size = 100;
    uint32_t cheat_button_gap = 100;
    int32_t panel_pos_x = INT32_MIN;
    int32_t panel_pos_y = INT32_MIN;
    std::map<std::string, std::string> keys;
};
Options& Opt();
const std::string& BootLanguage();
bool HasLanguages();
bool LanguageAvailable(const std::string& code);
inline std::atomic<float> g_fps{0};
inline std::atomic<bool> g_hide_hud{false};
inline std::atomic<float> g_zoom{1.0f};
inline std::atomic<bool> g_focus_paused{false};
inline std::atomic<uint32_t> g_focus_pauses{0};
void WaitWhileUnfocused();
bool HudShown();

}  // namespace game
