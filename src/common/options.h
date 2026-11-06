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
    uint32_t fmv_scale = 0;  // 4:3 films on a wider screen: 0 fill it, top and bottom cut; 1 kept 4:3
    // a look over the frame: 0 none, 1 and 2 a CRT (light, strong), 3 sharpen, 4 vibrant, 5 noir, 6 sepia,
    // 7 night vision, 8 VHS
    uint32_t screen_filter = 0;
    bool reload_blur = true;
    bool bloom = true;  // the glow added over bright parts of the frame (native/vulkan.cpp)
    bool fog = true;
    uint32_t anti_aliasing = 0;  // 0 off, 1 FXAA as the frame goes onto the screen, 2 SSAA: twice the resolution
    bool hdr = false;  // a tone map that brings back the colours the console's overbright look burns out
    uint32_t msaa = 0;  // samples a pixel of the scene, 0 off
    uint32_t draw_distance = 1;  // times the game's LOD and draw distances
    // the render resolution follows the frame rate (native/dynamic_resolution.cpp): down when it falls under the
    // target, up again once it holds the target this many seconds; within min and max, percent of the screen's
    bool dynamic_resolution = false;
    uint32_t dynamic_target_fps = 60;
    uint32_t dynamic_min_percent = 40;
    uint32_t dynamic_max_percent = 100;
    uint32_t dynamic_hold = 5;
    float mouse_sensitivity = 1.0f;
    float touch_sensitivity = 1.0f;  // the touch screen's look, apart from the mouse's
    bool invert_mouse = false;
    bool hold_aim = true;  // scopes too, keyboard and mouse
    uint32_t fov = 70;  // horizontal degrees on a 4:3 frame, 70 is the game's
    uint32_t viewmodel_fov = 70;  // the gun and arms, the same measure
    uint32_t walk_speed = 50;  // percent of running, while the walk key is held
    // a controller's sticks: the radial dead zone as percent of the travel, 0 the game's own, and what the travel
    // past it is scaled by; the left stick moves, the right looks
    uint32_t left_deadzone = 0;
    uint32_t right_deadzone = 0;
    float left_sensitivity = 1.0f;
    float right_sensitivity = 1.0f;
    uint32_t audio_output = 0;  // audio::kOutputAuto, audio::kOutputStereo, audio::kOutputSurround or kOutputMono
    std::string language = "int";  // one of kLanguages
    bool infinite_ammo = false;  // cheat: the player's magazine and reserve never go down
    // cheat: times the player's rate of fire, 1 the game's. The panel offers these discrete values via
    // kRapidFireLevels / kRapidFireNames in the cheats panel:
    //   OFF, 2X, 3X, 4X, 5X, 6X, 7X, 8X, 16X, 32X, 64X, 128X, 256X, 512X, INFINITE (999)
    // hooks.cpp (sub_001EEFD0) divides the wait between shots by rapid_fire: with INFINITE (999) the wait is
    // effectively zero → one projectile per frame (frame-rate continuous fire). With values 2..8 the wait is
    // divided the usual way, subject to the game's fire-mode pacing.
    uint32_t rapid_fire = 1;
    // cheat: rapid fire extra drives two things, INDEPENDENTLY:
    //
    //  (1) Slow-fire weapons — gated by this toggle AND rapid_fire > 1. The held weapon's animation is
    //      cleared at a fraction of frames proportional to rapid_fire (2X = every 4th frame, 8X = every
    //      frame) so the game accepts the next trigger pull sooner. RAPID FIRE OFF disables this branch.
    //      The weapons handled here:
    //        - RPG, GRL   single-shot launchers with slow reload
    //        - SNR, SN5   Walther 2000 bolt action (silenced / unsilenced)
    //        - BNS        M16A2 + M203: the M203 UNDER-BARREL GRENADE LAUNCHER's break-open / load / close
    //                     animation. The M16 primary fire is already fast via rapid_fire; the M203 alternate
    //                     fire is a single-shot launcher, and clearing its animation lets it fire
    //                     CONTINUOUSLY, WITHOUT PAUSE, WITHOUT DELAY, even though each grenade launches
    //                     individually. Requires INFINITE AMMO (the M203's alt ammo at +0xF8) so the
    //                     launcher does not run dry; INFINITE GRENADES is NOT needed for the M203 (that is
    //                     for hand grenades only).
    //
    //  (2) Hand grenades — gated by this toggle AND infinite_grenades ALONE. NO rapid_fire level required:
    //      grenades throw continuously, with no pause, no delay, no recovery window, even when RAPID FIRE
    //      is OFF. Each throw's release is detected the instant the count drops (sub_001E8830), and the
    //      throw animation, the held weapon's animation and the inventory busy flag are cleared right after
    //      the game's update (in ExtraRapidFire, cheats.cpp), so the next frame starts a fresh throw:
    //      ~2-3 throws per second at 60 fps (bounded by the throw animation's natural release frame).
    //
    // NOTE ON GRENADE RATE: neither the hand grenade throw rate nor the M203 fire rate scale with
    // rapid_fire level — both are bounded by their animation's natural release frames. What follows the
    // toggle is CONTINUOUS SPAM independent of the rapid_fire level. Scaling the rate would require
    // advancing the animation's time field each frame, which needs the animation controller's time-field
    // offset that this code does not currently know; writing to the wrong offset would corrupt the
    // animation state and crash the game.
    bool rapid_fire_extra = false;
    bool no_recoil = false;  // cheat: the player's shots do not kick the view
    // cheat: bullets fly straight from the muzzle (no spread cone) AND the weapon stays exactly on the aim
    // line during fire. Two things are zeroed when this is on, in addition to the spread floats at
    // weapon_data + 0x40 (weapons.cpp's ApplyNoBulletSpread):
    //   1. The gun's sway shares (hooks.cpp's SwayBegin): the gun snaps to the aim position every frame, so
    //      it does not trail the stick — no visual wobble while moving or firing. The gun is exactly on the
    //      crosshair, so every bullet leaves straight toward the crosshair point.
    //   2. The view recoil (hooks.cpp's RecoilDecay): the view does not kick up from shots, so the aim line
    //      does not drift off the crosshair between shots (same effect as NO RECOIL, but tied to this option
    //      so perfect accuracy also keeps the crosshair steady).
    // The fire animation still plays (a small muzzle climb, visual only), but the bullet direction is the
    // camera's, not the gun's, so all bullets hit the crosshair point. Most useful with a custom crosshair
    // (crosshair_* above) for precise aim. Turning this off restores the game's sway and recoil on the next
    // frame (SwayEnd restores the original shares; the recoil factor is computed normally again).
    bool no_bullet_spread = false;
    uint32_t game_speed = 100;  // cheat: percent of the game's speed, menus and films keep theirs
    bool god_mode = false;  // cheat: no hit takes the player's health
    bool one_hit_kill = false;  // cheat: any hit of the player's bullets kills
    bool infinite_grenades = false;  // cheat: throwing a grenade keeps the count
    bool infinite_medkits = false;  // cheat: using a medkit keeps the count (src/game/weapons.cpp)
    bool unlock_levels = false;  // cheat: every mission open on every difficulty, the save is left as it is
    bool unlock_black_ops = false;  // cheat: Black Ops open on the missions open on another difficulty, the same
    bool esp = false;  // cheat: the ESP master toggle; what it draws is picked by esp_display below
    // cheat: which ESP elements to draw. Bit 0 (1) = line from the bottom of the screen to the enemy's feet,
    // bit 1 (2) = health bar above its head. Values:
    //   0 = OFF        1 = LINE       2 = HEALTH       3 = LINE + HEALTH (default)
    // The health bar's fill colour is green above 66% of the reference max, yellow above 33%, red below;
    // the reference is kEnemyRefHealth in esp.cpp (250 HP = full bar). The bar's background uses the ESP
    // colour at 30% brightness, the fill uses its own green/yellow/red palette.
    uint32_t esp_display = 3;
    // cheat: colour of the ESP line and health bar background. 0..7 a preset from kEspPresets (WHITE, RED,
    // GREEN, CYAN, YELLOW, MAGENTA, ORANGE, PURPLE), 8 CUSTOM: the esp_red / esp_green / esp_blue below.
    // Stepping the preset writes its colour into those three fields, and any manual change of them switches
    // the preset back to CUSTOM, so the two always agree — the row shows a preset name while the number is
    // one of the eight, CUSTOM otherwise.
    uint32_t esp_color = 0;
    uint32_t esp_red = 224;    // 0..255, custom red    (224/255 ≈ 0.88, the soft white the ESP shipped with)
    uint32_t esp_green = 219;  // 0..255, custom green  (219/255 ≈ 0.86)
    uint32_t esp_blue = 209;   // 0..255, custom blue   (209/255 ≈ 0.82)
    // ---- crosshair ---------------------------------------------------------------------------------------
    // A crosshair drawn in the centre of the viewport, custom shape and colour (like ESP's line, it uses
    // the game's own untextured triangle batch at esp.cpp's kTriangles). Independent of the ESP master
    // toggle: crosshair draws even when ESP is off, and vice versa. Sizes are in "pixels at 480p" (the
    // ESP thickness measure) so they scale with the render resolution via the `unit` passed to DrawEsp.
    //
    // Shapes:  0 = + (cross)   1 = * (star)   2 = X (diagonal cross)   3 = DOT (small square)
    // The + and X styles are the two halves of the *; the * is both together.
    //
    // Colour:  same scheme as ESP. crosshair_color picks a preset from kEspPresets (WHITE, RED, GREEN,
    // CYAN, YELLOW, MAGENTA, ORANGE, PURPLE) or 8 (CUSTOM) reads crosshair_red / crosshair_green /
    // crosshair_blue. Stepping the preset writes its RGB into those fields (like ESP), and any manual
    // change of them switches the preset back to CUSTOM. Alpha is always 1 (fully opaque).
    bool crosshair = false;  // master toggle
    uint32_t crosshair_style = 0;  // 0=+, 1=*, 2=X, 3=DOT
    uint32_t crosshair_size = 8;  // pixels at 480p, the arm length from the gap
    uint32_t crosshair_thickness = 2;  // pixels at 480p, the arm's width
    uint32_t crosshair_gap = 4;  // pixels at 480p, the empty centre (0 = arms meet in the middle)
    uint32_t crosshair_color = 0;  // 0..7 preset, 8 CUSTOM (uses crosshair_red / green / blue below)
    uint32_t crosshair_red = 224;    // 0..255, custom red
    uint32_t crosshair_green = 219;  // 0..255, custom green
    uint32_t crosshair_blue = 209;   // 0..255, custom blue
    bool aimbot = false;  // cheat: aiming in turns the view to the body of the enemy nearest the crosshair
    // cheat: where the aimbot aims on the enemy. 0 the neck just under the chin (lethal, forgiving to bob),
    // 1 the chest centre (easier to land, less lethal), 2 the legs (slows a sprinting enemy without killing),
    // 3 the head (the headshot line, most lethal, most sensitive to the bob of a walking or crouching enemy)
    uint32_t aimbot_target = 0;
    // cheat: how strongly the aimbot pulls, percent of the angle to the target applied each frame. 15 the
    // default (a gentle assist the mouse still wins against), 100 a hard snap, 1 a barely-felt tug.
    uint32_t aimbot_pull = 15;
    // cheat: tenths of a degree inside which the aimbot does not turn. Larger leaves the crosshair freer so it
    // can be slid off one enemy onto the next; smaller keeps it locked on the current one. 15 = 1.5 degrees.
    uint32_t aimbot_deadzone = 15;
    // cheat panel UI:
    //
    //   cheat_panel_size    50..300  — overall scale of the whole panel (font, buttons, headings, window
    //                                 footprint). 100% is the historical layout; <100% makes the panel
    //                                 smaller and more compact; >100% makes it larger. Values above 100%
    //                                 may overflow the screen on phones (the user chose the size, they
    //                                 can lower it back). This row is the master size control; the two
    //                                 per-element rows below fine-tune the layout inside it.
    //   cheat_button_size   50..1000 — side of the "<" / ">" buttons (100 = standard frame height)
    //   cheat_button_gap     0..1000 — gap between a button and the value text (100 = standard spacing)
    //
    // The panel can also be dragged: hold the mouse on the title bar ("CHEATS" at the top) and move it
    // to any position. The drag is clamped so ~4 ems of the panel stay on screen at each edge, and the
    // new position is stored here so it survives a panel close/reopen and the session. A "Defaults" press
    // in the footer resets it back to the centered default.
    //
    // All three size settings and the position are edited/used in the CHEATS page's CHEAT UI section
    // (position is drag-driven); a RESET CHEAT UI row returns the three sizes to 100% in one press.
    // Applied live (no restart), persisted to settings like every other option.
    uint32_t cheat_panel_size = 100;
    uint32_t cheat_button_size = 100;
    uint32_t cheat_button_gap = 100;
    // The saved top-left position of the panel window, in screen pixels. INT32_MIN in both is the sentinel
    // for "never dragged yet — use the centered default computed from the viewport". DrawTitle writes the
    // current position here after each drag; DrawPanel reads it to restore the last position; the footer
    // "Defaults" button sets both back to INT32_MIN to recenter.
    int32_t panel_pos_x = INT32_MIN;
    int32_t panel_pos_y = INT32_MIN;
    std::map<std::string, std::string> keys;  // action id to key names, absent is the default
};
Options& Opt();
// the language setting as this run started, the language the game boots in
const std::string& BootLanguage();
bool HasLanguages();  // language.cpp
bool LanguageAvailable(const std::string& code);  // language.cpp: the LANGUAGE row offers it
inline std::atomic<float> g_fps{0};  // from the host, for the game's own counter
inline std::atomic<bool> g_hide_hud{false};  // the toggle HUD key: the gameplay HUD is not drawn
// the view's zoom as the look update scales the turn by (src/game/mouse.cpp): 1 none, about 0.74 a rifle's sights,
// a scope's levels under that (a half at X2, a quarter at X5)
inline std::atomic<float> g_zoom{1.0f};
inline std::atomic<bool> g_focus_paused{false};  // the game thread waits while the app is in the background
inline std::atomic<uint32_t> g_focus_pauses{0};  // focus pauses ended so far
void WaitWhileUnfocused();  // native/game_host.cpp: game thread, before the present
bool HudShown();  // src/game/hud.cpp: the player's HUD drew lately, a hint's pause too

}  // namespace game
