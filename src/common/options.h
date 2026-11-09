#pragma once
#include <atomic>
#include <cstdint>
#include <map>
#include <string>

namespace game {

inline constexpr const char* kLanguages[] = {"int", "fra", "deu", "ita", "esp", "ind"};

struct Options {
    bool skip_intros = false;
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
    uint32_t rapid_fire = 1;
    // cheat: rapid fire extra — a level like rapid_fire (1 = OFF, 2..999 = level).
    // Two independent uses:
    //  (1) Slow-fire weapons (RPG, GRL, SNR, SN5, BNS): requires rapid_fire_extra > 1 AND rapid_fire > 1.
    //      The animation clear rate is rapid_fire * rapid_fire_extra, capped at kMaxRapidLevel (8).
    //  (2) Hand grenades: requires rapid_fire_extra > 1 AND infinite_grenades. Throws continuously
    //      without pause, no delay, no recovery window — at ANY level > 1. The throw rate is
    //      bounded by the throw animation's natural release frame (~2-3 throws/sec at 60 fps).
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
    // health bar (or above the head when the health bar is off). Drawn in the same ESP colour.
    // The number is metres, computed as the 3D distance between the player's position and the
    // enemy's position. Capped at 999 (the display is 3 digits). Independent of the esp_display
    // line/health bits: it can show with line only, health only, both, or neither.
    bool esp_distance = false;
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
