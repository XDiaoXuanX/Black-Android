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
    //  (2) Hand grenades: requires rapid_fire_extra > 1 AND infinite_grenades. Throws continuously.
    uint32_t rapid_fire_extra = 1;
    // cheat: reload speed. 0 = DEFAULT (game's own speed), 1..10 = faster.
    //
    // Every player weapon's reload animation is sped up by scaling the animation's frame count
    // during the animation update. The scale factor for level N is 1/(1 + N*0.3):
    //   level 1  ≈ 1.3x faster    level 5  = 2.5x faster
    //   level 10 = 4.0x faster
    //
    // The scaling applies ONLY when the player's weapon is NOT firing (weapon state != 1).
    // Fire animations are excluded because their timing is controlled by kWeaponWait
    // (rapid_fire / rapid_fire_extra); speeding them up would clear g_idle_early incorrectly
    // and interrupt continuous fire. Non-fire animations (reload, weapon switch, idle) are
    // safe to speed up — their only effect is visual, and speeding them up makes them complete
    // sooner.
    //
    // Implementation: hooks.cpp's sub_0017B130 hook temporarily scales the animation clip's
    // kAnimationFrames field (0x1C) for the duration of the update call, then restores it. The
    // animation system reads this field to compute the clip's total duration; a shorter duration
    // means the animation reaches its end (and any action triggered at the end, like ammo
    // transfer during reload) sooner.
    uint32_t reload_speed = 0;  // 0 = DEFAULT, 1..10 = faster reload
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
    bool esp_distance = false;
    bool esp_arrow = false;
    uint32_t esp_arrow_mode = 2;
    uint32_t esp_arrow_size = 4;
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
