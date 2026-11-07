// the cheats panel, on the touch controls' gear button or F10
#include <algorithm>
#include <atomic>
#include <cctype>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <iterator>
#include <map>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include <SDL3/SDL.h>
#include <imgui.h>
#include <imgui_internal.h>

#include "app/app.h"
#include "audio/output.h"
#include "common/keys.h"
#include "common/menu.h"
#include "common/mouse_keyboard.h"
#include "common/options.h"
#include "common/translations.h"
#include "game/weapons.h"

namespace gyro {
uint32_t CurrentMode();
const char* ModeName();
void SetMode(uint32_t mode);
float Sensitivity();
void SetSensitivity(float s);
bool InvertX();
bool InvertY();
void SetInvert(bool x, bool y);
}  // namespace gyro

namespace touch {
const char* LayoutName();
void StepLayout(int step);
uint32_t ControlsOpacity();
void StepOpacity(int step);
uint32_t ControlsSize();
void StepSize(int step);
void EditLayout();
const char* SkinName();
void StepSkin(int step);
const char* StickModeName();
void StepStickMode(int step);
}  // namespace touch

namespace dynres {
uint32_t Percent();
}  // namespace dynres

namespace host {
void CheckForUpdates();
bool StartupCheck();
void SetStartupCheck(bool on);
}  // namespace host

namespace app {

Settings DefaultSettings();

namespace {

std::atomic<bool> g_open{false};

// ---- language restart confirmation --------------------------------------------------------------
// Set when the user steps the LANGUAGE row to a code that is not the language this process booted
// with: the game only reads its language at boot, so a change needs a fresh process. The modal in
// DrawLanguageRestartDialog offers Yes (Restart) — Save() then a SDL_EVENT_QUIT that the main loop
// catches and quits from — or No (Cancel) — revert to the code stored here and Save() again.
// Escape and closing the panel both mean Cancel.
bool g_language_restart_open = false;
std::string g_language_previous_code;   // the code before the step, restored on Cancel
std::string g_language_pending_label;   // the new language's display name, shown in the modal

constexpr ImVec4 kPanelBg{0.035f, 0.03f, 0.025f, 0.94f};
constexpr ImVec4 kBorder{0.72f, 0.58f, 0.30f, 0.85f};
constexpr ImVec4 kText{0.90f, 0.88f, 0.84f, 1.0f};
constexpr ImVec4 kHighlight{0.30f, 0.23f, 0.10f, 1.0f};
constexpr ImVec4 kHeading{0.86f, 0.69f, 0.34f, 1.0f};
constexpr ImVec4 kDim{0.62f, 0.58f, 0.50f, 1.0f};
constexpr ImVec4 kTab{0.12f, 0.10f, 0.07f, 1.0f};
constexpr ImVec4 kTabHovered{0.20f, 0.16f, 0.09f, 1.0f};
constexpr ImVec4 kButton{0.17f, 0.14f, 0.08f, 1.0f};
constexpr ImVec4 kButtonHovered{0.40f, 0.31f, 0.14f, 1.0f};
constexpr ImVec4 kLine{0.86f, 0.69f, 0.34f, 0.35f};
constexpr std::pair<ImGuiCol, ImVec4> kPanelColors[] = {
    {ImGuiCol_WindowBg, kPanelBg}, {ImGuiCol_Border, kBorder}, {ImGuiCol_Text, kText},
    {ImGuiCol_Header, kHighlight}, {ImGuiCol_HeaderHovered, kHighlight}, {ImGuiCol_HeaderActive, kHighlight},
    {ImGuiCol_Button, kButton}, {ImGuiCol_ButtonHovered, kButtonHovered}, {ImGuiCol_ButtonActive, kButtonHovered},
    {ImGuiCol_FrameBg, kTab}, {ImGuiCol_FrameBgHovered, kTabHovered}, {ImGuiCol_FrameBgActive, kHighlight},
    {ImGuiCol_ScrollbarGrab, kHighlight}, {ImGuiCol_ScrollbarGrabHovered, kButtonHovered},
    {ImGuiCol_ScrollbarGrabActive, kHeading}, {ImGuiCol_Separator, kLine}, {ImGuiCol_NavHighlight, kHeading},
};

constexpr uint32_t kAnisoValues[] = {0, 2, 4, 8, 16};
const char* const kAnisoNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "16x"};
constexpr uint32_t kDistances[] = {1, 2, 4, 8, 12, 16};
const char* const kDistanceNames[] = {"GAME DEFAULT", "2x", "4x", "8x", "12x", "16x"};
constexpr uint32_t kMsaaValues[] = {0, 2, 4, 8};
const char* const kMsaaNames[] = {"OFF", "2x", "4x", "8x"};
constexpr uint32_t kAntiAliasings[] = {0, 1, 2};
const char* const kAntiAliasingNames[] = {"OFF", "FXAA", "SSAA 2X"};
constexpr uint32_t kFmvScales[] = {0, 1};
const char* const kFmvScaleNames[] = {"FILL", "4:3"};
constexpr uint32_t kScreenFilters[] = {0, 1, 2, 3, 4, 5, 6, 7, 8};
const char* const kScreenFilterNames[] = {"OFF",  "CRT LIGHT", "CRT STRONG",   "SHARPEN", "VIBRANT",
                                          "NOIR", "SEPIA",     "NIGHT VISION", "VHS"};
constexpr uint32_t kAudioOutputs[] = {audio::kOutputAuto, audio::kOutputMono, audio::kOutputStereo,
                                      audio::kOutputSurround};
const char* const kAudioOutputNames[] = {"AUTOMATIC", "MONO", "2.0 STEREO", "5.1 SURROUND"};
const char* const kLanguageNames[] = {"ENGLISH", "FRENCH", "GERMAN", "ITALIAN", "SPANISH",
                                      "INDONESIAN"};  // as game::kLanguages
const char* const kAimbotTargetNames[] = {"NECK", "BODY", "LEGS", "HEAD"};

constexpr uint32_t kRapidFireLevels[] = {
    1, 2, 3, 4, 5, 6, 7, 8, 16, 32, 64, 128, 256, 512, 999,
};
const char* const kRapidFireNames[] = {
    "OFF",  "2X",   "3X",   "4X",   "5X",   "6X",   "7X",  "8X",
    "16X",  "32X",  "64X",  "128X", "256X", "512X", "INFINITE",
};

const char* const kEspDisplayNames[] = {
    "OFF",             // 0
    "LINE",            // 1
    "HEALTH",          // 2
    "LINE + HEALTH",   // 3 (default)
};

struct EspPreset {
    const char* name;
    float r, g, b;
};
constexpr EspPreset kEspPresets[] = {
    {"WHITE", 0.88f, 0.86f, 0.82f},
    {"RED", 1.00f, 0.20f, 0.20f},
    {"GREEN", 0.20f, 1.00f, 0.30f},
    {"CYAN", 0.20f, 0.90f, 1.00f},
    {"YELLOW", 1.00f, 0.85f, 0.20f},
    {"MAGENTA", 1.00f, 0.30f, 1.00f},
    {"ORANGE", 1.00f, 0.55f, 0.10f},
    {"PURPLE", 0.70f, 0.40f, 1.00f},
};
constexpr size_t kEspPresetCount = sizeof(kEspPresets) / sizeof(kEspPresets[0]);

const char* const kCrosshairStyleNames[] = {"+ (CROSS)", "* (STAR)", "X (DIAGONAL)", "DOT"};

constexpr uint32_t kFpsCaps[] = {30, 60, 75, 90, 120, 144, 165, 240, 360, kFpsCapRefresh, 0};

int StepIndex(int index, int step, int count) {
    if (count <= 0) return 0;
    index = (index + step) % count;
    return index < 0 ? index + count : index;
}

int NearestIndex(const uint32_t* values, int count, uint32_t value) {
    int best = 0;
    uint32_t best_d = UINT32_MAX;
    for (int i = 0; i < count; i++) {
        uint32_t d = values[i] > value ? values[i] - value : value - values[i];
        if (d < best_d) {
            best_d = d;
            best = i;
        }
    }
    return best;
}

template <size_t N>
int StepTable(const uint32_t (&values)[N], uint32_t& value, int step) {
    int i = NearestIndex(values, int(N), value);
    if (step) {
        i = StepIndex(i, step, int(N));
        value = values[i];
    }
    return i;
}

int StepRange(int value, int lo, int hi, int inc, int step) {
    int count = (hi - lo) / inc + 1;
    return lo + inc * StepIndex((std::clamp(value, lo, hi) - lo) / inc, step, count);
}

// ---- panel drag-anywhere ----------------------------------------------------------------------------------
static bool g_panel_dragging = false;
static ImVec2 g_panel_drag_origin;
static ImVec2 g_panel_mouse_origin;
static bool g_panel_drag_moved = false;

// ---- collapsible sections ----------------------------------------------------------------------------------
std::map<std::string, bool> g_collapsed;

bool Section(const char* title) {
    float em = ImGui::GetFontSize();
    ImGui::Dummy(ImVec2(0, em * 0.3f));
    std::string key(title);
    bool& collapsed = g_collapsed.try_emplace(key, true).first->second;

    ImGui::PushID(title);
    ImVec2 start = ImGui::GetCursorScreenPos();
    float width = ImGui::GetContentRegionAvail().x;
    float height = em * 1.3f;
    if (ImGui::InvisibleButton("##head", ImVec2(width, height))) collapsed = !collapsed;
    if (ImGui::IsItemHovered()) {
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                   ImGui::GetColorU32(ImGuiCol_Header));
    }
    ImDrawList* draw = ImGui::GetWindowDrawList();
    ImU32 colour = ImGui::GetColorU32(kHeading);
    float cx = start.x + em * 0.6f;
    float cy = start.y + height * 0.5f;
    float sz = em * 0.35f;
    if (collapsed) {
        draw->AddTriangleFilled(ImVec2(cx - sz * 0.5f, cy - sz), ImVec2(cx - sz * 0.5f, cy + sz),
                                 ImVec2(cx + sz * 0.7f, cy), colour);
    } else {
        draw->AddTriangleFilled(ImVec2(cx - sz, cy - sz * 0.5f), ImVec2(cx + sz, cy - sz * 0.5f),
                                 ImVec2(cx, cy + sz * 0.7f), colour);
    }
    float text_y = start.y + (height - ImGui::GetTextLineHeight()) * 0.5f;
    draw->AddText(ImVec2(start.x + em * 1.4f, text_y), colour, Ui(title).c_str());
    ImGui::SetCursorScreenPos(ImVec2(start.x, start.y + height));
    ImGui::PopID();

    ImGui::Separator();
    if (!collapsed) ImGui::Dummy(ImVec2(0, em * 0.1f));
    return !collapsed;
}

constexpr float kValueEms = 12.0f;

struct RowBox {
    float em;
    float row_w;
    float row_h;
    float value_w;
    float block_w;
    float row_x;
    float row_y;
};

RowBox MeasureRow() {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox box;
    box.em = ImGui::GetFontSize();
    box.row_w = ImGui::GetContentRegionAvail().x;
    box.row_h = ImGui::GetFrameHeight();
    box.value_w = box.em * kValueEms;
    box.block_w = box.row_h * 2 + box.value_w + style.ItemSpacing.x * 2;
    box.row_x = ImGui::GetCursorPosX();
    box.row_y = ImGui::GetCursorPosY();
    return box;
}

void RowLabel(const RowBox& box, const char* label) {
    ImGui::SetCursorPos(ImVec2(box.row_x + ImGui::GetStyle().ItemInnerSpacing.x, box.row_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Ui(label).c_str());
}

void EndRow(const RowBox& box) {
    ImGui::SetCursorPos(ImVec2(box.row_x, box.row_y));
    ImGui::Dummy(ImVec2(box.row_w, box.row_h));
}

void FillItem() {
    ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                              ImGui::GetColorU32(ImGuiCol_Header));
}

int OptionRow(const char* label, const char* value) {
    const ImGuiStyle& style = ImGui::GetStyle();
    const game::Options& opts = game::Opt();
    float em = ImGui::GetFontSize();
    float base_h = ImGui::GetFrameHeight();
    float size_pct = float(std::clamp(opts.cheat_button_size, 50u, 1000u)) / 100.0f;
    float gap_pct = float(std::clamp(opts.cheat_button_gap, 0u, 1000u)) / 100.0f;

    RowBox box;
    box.em = em;
    box.row_w = ImGui::GetContentRegionAvail().x;
    box.value_w = em * kValueEms;

    float btn_size_raw = base_h * size_pct;
    float btn_size = std::min(btn_size_raw, box.row_w * 0.25f);
    float gap = style.ItemSpacing.x * gap_pct;

    box.row_h = std::max(base_h, btn_size);
    box.block_w = btn_size * 2 + box.value_w + gap * 2;
    box.row_x = ImGui::GetCursorPosX();
    box.row_y = ImGui::GetCursorPosY();

    float text_y = box.row_y + (box.row_h - base_h) * 0.5f;
    float btn_y = box.row_y + (box.row_h - btn_size) * 0.5f;

    ImGui::PushID(label);
    int step = 0;
    ImGui::Selectable("##row", false, ImGuiSelectableFlags_AllowOverlap, ImVec2(box.row_w, box.row_h));
    if (ImGui::IsItemFocused()) {
        FillItem();
        if (ImGui::IsKeyPressed(ImGuiKey_LeftArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadLeft)) step = -1;
        if (ImGui::IsKeyPressed(ImGuiKey_RightArrow) || ImGui::IsKeyPressed(ImGuiKey_GamepadDpadRight)) step = 1;
    }
    ImGui::SetCursorPos(ImVec2(box.row_x + style.ItemInnerSpacing.x, text_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(Ui(label).c_str());

    ImGui::PushItemFlag(ImGuiItemFlags_NoNav | ImGuiItemFlags_ButtonRepeat, true);
    float block_x = box.row_x + box.row_w - box.block_w;

    ImGui::SetCursorPos(ImVec2(block_x, btn_y));
    if (ImGui::Button("<", ImVec2(btn_size, btn_size))) step = -1;

    float value_x = block_x + btn_size + gap;
    float pad = (box.value_w - ImGui::CalcTextSize(value).x) * 0.5f;
    ImGui::SetCursorPos(ImVec2(value_x + std::max(pad, 0.0f), text_y));
    ImGui::AlignTextToFramePadding();
    ImGui::TextUnformatted(value);

    float btn2_x = value_x + box.value_w + gap;
    ImGui::SetCursorPos(ImVec2(btn2_x, btn_y));
    if (ImGui::Button(">", ImVec2(btn_size, btn_size))) step = 1;

    ImGui::PopItemFlag();
    ImGui::PopID();
    EndRow(box);
    return step;
}

const char* OnOff(bool on) { return on ? "ON" : "OFF"; }
void Flip(bool& on, int step) { if (step) on = !on; }

void Changed() {
    audio::SetOutputMode(game::Opt().audio_output);
    Save();
}

// Called from the modal's Yes (Restart) button. The language is already saved (Changed() runs when
// the row steps), this just makes sure everything else is committed too before the process exits.
// The main loop owns the actual SDL_Quit; pushing SDL_EVENT_QUIT is the cleanest cross-platform
// request. On Android the activity finishes, the launcher can relaunch; on desktop the process ends
// and a relaunch picks the new language up at boot.
void RestartForLanguage() {
    Changed();
    SDL_Event ev;
    SDL_zero(ev);
    ev.type = SDL_EVENT_QUIT;
    SDL_PushEvent(&ev);
}

enum RowId { kRenderResolution, kMaxFps, kVsync, kFpsCounter, kAniso, kReloadBlur, kSkipIntros, kSensitivity,
             kInvertMouse, kWalkSpeed, kFov, kViewmodelFov, kHoldAim, kDrawDistance, kAudioOutput, kLanguage,
             kFmvScale, kGyro, kGyroSensitivity, kTouchLayout, kTouchOpacity, kTouchSize, kTouchEdit,
             kInfiniteAmmo, kRapidFire, kRapidFireExtra, kNoRecoil, kNoBulletSpread, kGameSpeed, kGodMode, kOneHitKill,
             kInfiniteGrenades, kCheckUpdates, kTouchSensitivity, kLeftDeadzone, kRightDeadzone, kLeftSensitivity,
             kRightSensitivity, kScreenFilter, kGyroInvertX, kGyroInvertY, kUnlockLevels, kUnlockBlackOps, kTouchSkin,
             kCredit, kBloom, kFog, kAntiAliasing, kMsaa, kHdr, kEsp, kEspDisplay, kEspColor,
             kEspRed, kEspGreen, kEspBlue, kCrosshair, kCrosshairStyle, kCrosshairSize, kCrosshairThickness,
             kCrosshairGap, kCrosshairColor, kCrosshairRed, kCrosshairGreen, kCrosshairBlue,
             kAimbot, kAimbotTarget, kAimbotPull, kAimbotDeadzone, kTouchStick,
             kDynamicRes, kDynamicTarget, kDynamicMin, kDynamicMax, kDynamicHold, kAutoUpdate, kInfiniteMedkits,
             kCheatPanelSize, kCheatButtonSize, kCheatButtonGap, kCheatReset };

using rows::Kind;
using rows::Row;

constexpr Row kGraphicsRows[] = {
    {Kind::Heading, -1, "DISPLAY"},
    {Kind::Value, kRenderResolution, "RENDER RESOLUTION"},
    {Kind::Value, kFov, "FIELD OF VIEW"},
    {Kind::Value, kViewmodelFov, "VIEWMODEL FIELD OF VIEW"},
    {Kind::Value, kFmvScale, "FMV SCALE"},
    {Kind::Value, kScreenFilter, "SCREEN FILTER"},
    {Kind::Heading, -1, "PERFORMANCE"},
    {Kind::Value, kVsync, "VERTICAL SYNC"},
    {Kind::Value, kMaxFps, "MAX FRAME RATE"},
    {Kind::Value, kFpsCounter, "FPS COUNTER"},
    {Kind::Value, kDynamicRes, "DYNAMIC RESOLUTION"},
    {Kind::Value, kDynamicTarget, "TARGET FRAME RATE"},
    {Kind::Value, kDynamicMin, "MIN RESOLUTION"},
    {Kind::Value, kDynamicMax, "MAX RESOLUTION"},
    {Kind::Value, kDynamicHold, "HOLD BEFORE RAISING"},
    {Kind::Heading, -1, "QUALITY"},
    {Kind::Value, kAniso, "ANISOTROPIC FILTERING"},
    {Kind::Value, kDrawDistance, "DRAW DISTANCE"},
    {Kind::Value, kReloadBlur, "RELOAD BLUR"},
    {Kind::Value, kBloom, "BLOOM"},
    {Kind::Value, kFog, "FOG"},
    {Kind::Value, kAntiAliasing, "ANTI-ALIASING"},
    {Kind::Value, kMsaa, "MSAA"},
    {Kind::Value, kHdr, "HDR"},
};

constexpr Row kTouchRows[] = {
    {Kind::Heading, -1, "TOUCH"},
    {Kind::Value, kTouchSensitivity, "TOUCH SENSITIVITY"},
    {Kind::Value, kTouchLayout, "CONTROLS LAYOUT"},
    {Kind::Value, kTouchOpacity, "CONTROLS OPACITY"},
    {Kind::Value, kTouchSize, "CONTROLS SIZE"},
    {Kind::Value, kTouchSkin, "BUTTON STYLE"},
    {Kind::Value, kTouchStick, "ANALOG STICK"},
    {Kind::Value, kTouchEdit, "EDIT CONTROLS POSITION"},
    {Kind::Heading, -1, "MOTION CONTROLS"},
    {Kind::Value, kGyro, "GYROSCOPE"},
    {Kind::Value, kGyroSensitivity, "GYRO SENSITIVITY"},
    {Kind::Value, kGyroInvertX, "GYRO INVERT X"},
    {Kind::Value, kGyroInvertY, "GYRO INVERT Y"},
};

constexpr Row kControllerRows[] = {
    {Kind::Value, kLeftDeadzone, "LEFT STICK DEADZONE"},
    {Kind::Value, kRightDeadzone, "RIGHT STICK DEADZONE"},
    {Kind::Value, kLeftSensitivity, "LEFT STICK SENSITIVITY"},
    {Kind::Value, kRightSensitivity, "RIGHT STICK SENSITIVITY"},
};

constexpr Row kGeneralRows[] = {
    {Kind::Value, kLanguage, "LANGUAGE"},
    {Kind::Value, kSkipIntros, "SKIP INTROS"},
    {Kind::Value, kAudioOutput, "AUDIO OUTPUT"},
    {Kind::Heading, -1, "UPDATES"},
    {Kind::Value, kCheckUpdates, "CHECK FOR UPDATES"},
    {Kind::Value, kAutoUpdate, "STARTUP UPDATE CHECK"},
};

constexpr Row kCheatRows[] = {
    {Kind::Heading, -1, "WEAPONS"},
    {Kind::Value, kInfiniteAmmo, "INFINITE AMMO"},
    {Kind::Value, kInfiniteGrenades, "INFINITE GRENADES"},
    {Kind::Value, kRapidFire, "RAPID FIRE"},
    {Kind::Value, kRapidFireExtra, "RAPID FIRE EXTRA"},
    {Kind::Value, kNoRecoil, "NO RECOIL"},
    {Kind::Value, kNoBulletSpread, "NO BULLET SPREAD"},
    {Kind::Heading, -1, "PLAYER"},
    {Kind::Value, kGodMode, "GOD MODE"},
    {Kind::Value, kInfiniteMedkits, "INFINITE MEDKITS"},
    {Kind::Value, kOneHitKill, "ONE-HIT KILL"},
    {Kind::Heading, -1, "ESP"},
    {Kind::Value, kEsp, "ESP MODE"},
    {Kind::Value, kEspDisplay, "ESP MODE TARGET"},
    {Kind::Value, kEspColor, "ESP COLOR"},
    {Kind::Value, kEspRed, "ESP RED"},
    {Kind::Value, kEspGreen, "ESP GREEN"},
    {Kind::Value, kEspBlue, "ESP BLUE"},
    {Kind::Heading, -1, "CROSSHAIR"},
    {Kind::Value, kCrosshair, "CROSSHAIR"},
    {Kind::Value, kCrosshairStyle, "CROSSHAIR STYLE"},
    {Kind::Value, kCrosshairSize, "CROSSHAIR SIZE"},
    {Kind::Value, kCrosshairThickness, "CROSSHAIR THICKNESS"},
    {Kind::Value, kCrosshairGap, "CROSSHAIR GAP"},
    {Kind::Value, kCrosshairColor, "CROSSHAIR COLOR"},
    {Kind::Value, kCrosshairRed, "CROSSHAIR RED"},
    {Kind::Value, kCrosshairGreen, "CROSSHAIR GREEN"},
    {Kind::Value, kCrosshairBlue, "CROSSHAIR BLUE"},
    {Kind::Heading, -1, "AIM ASSIST"},
    {Kind::Value, kAimbot, "AIMBOT (WHILE AIMING IN)"},
    {Kind::Value, kAimbotTarget, "AIMBOT TARGET"},
    {Kind::Value, kAimbotPull, "AIMBOT PULL"},
    {Kind::Value, kAimbotDeadzone, "AIMBOT DEADZONE"},
    {Kind::Heading, -1, "GAME"},
    {Kind::Value, kGameSpeed, "GAME SPEED"},
    {Kind::Value, kUnlockLevels, "UNLOCK ALL MISSIONS"},
    {Kind::Value, kUnlockBlackOps, "UNLOCK BLACK OPS"},
};

constexpr Row kDebugRows[] = {
    {Kind::Value, kCheatPanelSize, "PANEL UI SIZE"},
    {Kind::Value, kCheatButtonSize, "PANEL BUTTON SIZE"},
    {Kind::Value, kCheatButtonGap, "PANEL BUTTON GAP"},
    {Kind::Value, kCheatReset, "RESET PANEL UI"},
};

struct Credit {
    const char* label;
    const char* value;
    const char* url;
};
constexpr Credit kCredits[] = {
    {"CONTRIBUTORS", nullptr, nullptr},
    {"Rangga Casper", "ANDROID PORT", "https://github.com/RanggaCasper"},
    {"DATFOOLDIVE", "ANY WEAPON IN ANY LEVEL", "https://github.com/datfooldive"},
    {"BAGASROMADON", "BUTTON SKIN", "https://github.com/bagasromadon"},
    {"Zukiru", "HDR SHADER", nullptr},
    {"UNKNUW", "SUPPRESSOR + MEDKITS", "https://github.com/UnknuwProjects"},
    {"BUILT WITH", nullptr, nullptr},
    {"XBOXXBE-RECOMP", "TGP482", nullptr},
    {"SDL3, GLSLANG, TOML++", "LIBRARIES", nullptr},
    {"DEAR IMGUI", "LIBRARY", nullptr},
    {"XEMU, CXBX-RELOADED", "REFERENCES", nullptr},
    {"LINKS", nullptr, nullptr},
    {"YOUTUBE", "OPEN", "https://youtube.com/c/@RanggaCasper"},
    {"GITHUB", "OPEN", "https://github.com/RanggaCasper"},
    {"SUPPORT", "OPEN", "https://sociabuzz.com/racas/tribe"},
};

const std::vector<Row>& CreditRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out;
        for (const Credit& c : kCredits) out.push_back({c.value ? Kind::Value : Kind::Heading, kCredit, c.label});
        return out;
    }();
    return list;
}

const Credit* CreditOf(const Row& row) {
    for (const Credit& c : kCredits)
        if (row.label == c.label) return &c;
    return nullptr;
}

const std::vector<Row>& InputRows() {
    static const std::vector<Row> list = [] {
        std::vector<Row> out = {{Kind::Heading, -1, "MOUSE"},
                                {Kind::Value, kSensitivity, "MOUSE SENSITIVITY"},
                                {Kind::Value, kInvertMouse, "INVERT LOOK"},
                                {Kind::Value, kHoldAim, "HOLD TO AIM"},
                                {Kind::Heading, -1, "KEYBOARD"}};
        size_t count = 0;
        const input::Action* actions = input::Actions(&count);
        for (size_t i = 0; i < count; i++) out.push_back({Kind::Bind, int(i), actions[i].label});
        out.push_back({Kind::Value, kWalkSpeed, "WALK SPEED"});
        return out;
    }();
    return list;
}

bool* ToggleFlag(Settings& s, int id) {
    game::Options& o = game::Opt();
    switch (id) {
        case kVsync: return &s.vsync;
        case kFpsCounter: return &o.fps_counter;
        case kReloadBlur: return &o.reload_blur;
        case kBloom: return &o.bloom;
        case kFog: return &o.fog;
        case kHdr: return &o.hdr;
        case kSkipIntros: return &o.skip_intros;
        case kInvertMouse: return &o.invert_mouse;
        case kHoldAim: return &o.hold_aim;
        case kInfiniteAmmo: return &o.infinite_ammo;
        case kRapidFireExtra: return &o.rapid_fire_extra;
        case kNoRecoil: return &o.no_recoil;
        case kNoBulletSpread: return &o.no_bullet_spread;
        case kGodMode: return &o.god_mode;
        case kInfiniteMedkits: return &o.infinite_medkits;
        case kOneHitKill: return &o.one_hit_kill;
        case kInfiniteGrenades: return &o.infinite_grenades;
        case kEsp: return &o.esp;
        case kCrosshair: return &o.crosshair;
        case kUnlockLevels: return &o.unlock_levels;
        case kUnlockBlackOps: return &o.unlock_black_ops;
        case kAimbot: return &o.aimbot;
        case kDynamicRes: return &o.dynamic_resolution;
        default: return nullptr;
    }
}

constexpr int kFovMin = 45;
constexpr int kFovMax = 140;
constexpr uint32_t kGameFov = 70;

void FovText(uint32_t fov, char (&text)[64]) {
    std::snprintf(text, sizeof(text), fov == kGameFov ? "%u (DEFAULT)" : "%u", fov);
}

constexpr uint32_t kDynamicTargets[] = {30, 45, 60, 90, 120};

void PercentText(uint32_t percent, char (&text)[64]) {
    Mode out = Output();
    std::snprintf(text, sizeof(text), "%u%% (%ux%u)", percent, out.width * percent / 100, out.height * percent / 100);
}

bool StepDisplayRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kRenderResolution: {
            if (step) s.render_percent = uint32_t(StepRange(int(s.render_percent), 20, 200, 5, step));
            PercentText(o.dynamic_resolution ? dynres::Percent() : s.render_percent, text);
            return true;
        }
        case kDynamicTarget:
            std::snprintf(text, sizeof(text), "%u FPS", kDynamicTargets[StepTable(kDynamicTargets, o.dynamic_target_fps, step)]);
            return true;
        case kDynamicMin:
            if (step) o.dynamic_min_percent = uint32_t(StepRange(int(o.dynamic_min_percent), 20, int(o.dynamic_max_percent), 5, step));
            PercentText(o.dynamic_min_percent, text);
            return true;
        case kDynamicMax:
            if (step) o.dynamic_max_percent = uint32_t(StepRange(int(o.dynamic_max_percent), int(o.dynamic_min_percent), 200, 5, step));
            PercentText(o.dynamic_max_percent, text);
            return true;
        case kDynamicHold:
            if (step) o.dynamic_hold = uint32_t(StepRange(int(o.dynamic_hold), 1, 30, 1, step));
            std::snprintf(text, sizeof(text), "%u S", o.dynamic_hold);
            return true;
        case kFmvScale:
            std::snprintf(text, sizeof(text), "%s", kFmvScaleNames[StepTable(kFmvScales, o.fmv_scale, step)]);
            return true;
        case kAntiAliasing:
            std::snprintf(text, sizeof(text), "%s", kAntiAliasingNames[StepTable(kAntiAliasings, o.anti_aliasing, step)]);
            return true;
        case kMsaa:
           std::snprintf(text, sizeof(text), "%s", kMsaaNames[StepTable(kMsaaValues, o.msaa, step)]);
           return true;
        case kScreenFilter:
            std::snprintf(text, sizeof(text), "%s", kScreenFilterNames[StepTable(kScreenFilters, o.screen_filter, step)]);
            return true;
        case kFov:
            if (step) o.fov = uint32_t(StepRange(int(o.fov), kFovMin, kFovMax, 1, step));
            FovText(o.fov, text);
            return true;
        case kViewmodelFov:
            if (step) o.viewmodel_fov = uint32_t(StepRange(int(o.viewmodel_fov), kFovMin, kFovMax, 1, step));
            FovText(o.viewmodel_fov, text);
            return true;
        case kMaxFps: {
            uint32_t cap = kFpsCaps[StepTable(kFpsCaps, s.fps_cap, step)];
            if (cap == kFpsCapRefresh) std::snprintf(text, sizeof(text), "REFRESH RATE");
            else if (cap) std::snprintf(text, sizeof(text), "%u", cap);
            else std::snprintf(text, sizeof(text), "UNLIMITED (DEFAULT)");
            return true;
        }
        default:
            return false;
    }
}

bool StepGraphicsRow(int id, int step, Settings& s, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAniso:
            std::snprintf(text, sizeof(text), "%s", kAnisoNames[StepTable(kAnisoValues, s.aniso, step)]);
            return true;
        case kDrawDistance:
            std::snprintf(text, sizeof(text), "%s", kDistanceNames[StepTable(kDistances, o.draw_distance, step)]);
            return true;
        default:
            return false;
    }
}

bool StepCheatRow(int id, int step, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kRapidFire: {
            int i = StepTable(kRapidFireLevels, o.rapid_fire, step);
            std::snprintf(text, sizeof(text), "%s", kRapidFireNames[i]);
            return true;
        }
        case kEspDisplay:
            if (step) o.esp_display = uint32_t(StepRange(int(o.esp_display), 0, 3, 1, step));
            std::snprintf(text, sizeof(text), "%s", kEspDisplayNames[std::clamp(o.esp_display, 0u, 3u)]);
            return true;
        case kEspColor: {
            uint32_t preset = std::clamp(o.esp_color, 0u, uint32_t(kEspPresetCount));
            if (step) {
                preset = uint32_t(StepIndex(int(preset), step, int(kEspPresetCount) + 1));
                o.esp_color = preset;
                if (preset < kEspPresetCount) {
                    const EspPreset& p = kEspPresets[preset];
                    o.esp_red = uint32_t(p.r * 255.0f + 0.5f);
                    o.esp_green = uint32_t(p.g * 255.0f + 0.5f);
                    o.esp_blue = uint32_t(p.b * 255.0f + 0.5f);
                }
            }
            std::snprintf(text, sizeof(text), "%s",
                          preset < kEspPresetCount ? kEspPresets[preset].name : "CUSTOM");
            return true;
        }
        case kEspRed:
        case kEspGreen:
        case kEspBlue: {
            uint32_t& channel = id == kEspRed ? o.esp_red
                              : id == kEspGreen ? o.esp_green
                              : o.esp_blue;
            if (step) {
                channel = uint32_t(StepRange(int(channel), 0, 255, 1, step));
                o.esp_color = uint32_t(kEspPresetCount);
            }
            std::snprintf(text, sizeof(text), "%u", channel);
            return true;
        }
        case kCrosshairStyle:
            if (step) o.crosshair_style = uint32_t(StepRange(int(o.crosshair_style), 0, 3, 1, step));
            std::snprintf(text, sizeof(text), "%s", kCrosshairStyleNames[std::clamp(o.crosshair_style, 0u, 3u)]);
            return true;
        case kCrosshairSize:
            if (step) o.crosshair_size = uint32_t(StepRange(int(o.crosshair_size), 2, 32, 1, step));
            std::snprintf(text, sizeof(text), "%u", o.crosshair_size);
            return true;
        case kCrosshairThickness:
            if (step) o.crosshair_thickness = uint32_t(StepRange(int(o.crosshair_thickness), 1, 8, 1, step));
            std::snprintf(text, sizeof(text), "%u", o.crosshair_thickness);
            return true;
        case kCrosshairGap:
            if (step) o.crosshair_gap = uint32_t(StepRange(int(o.crosshair_gap), 0, 20, 1, step));
            std::snprintf(text, sizeof(text), "%u", o.crosshair_gap);
            return true;
        case kCrosshairColor: {
            uint32_t preset = std::clamp(o.crosshair_color, 0u, uint32_t(kEspPresetCount));
            if (step) {
                preset = uint32_t(StepIndex(int(preset), step, int(kEspPresetCount) + 1));
                o.crosshair_color = preset;
                if (preset < kEspPresetCount) {
                    const EspPreset& p = kEspPresets[preset];
                    o.crosshair_red = uint32_t(p.r * 255.0f + 0.5f);
                    o.crosshair_green = uint32_t(p.g * 255.0f + 0.5f);
                    o.crosshair_blue = uint32_t(p.b * 255.0f + 0.5f);
                }
            }
            std::snprintf(text, sizeof(text), "%s",
                          preset < kEspPresetCount ? kEspPresets[preset].name : "CUSTOM");
            return true;
        }
        case kCrosshairRed:
        case kCrosshairGreen:
        case kCrosshairBlue: {
            uint32_t& channel = id == kCrosshairRed ? o.crosshair_red
                              : id == kCrosshairGreen ? o.crosshair_green
                              : o.crosshair_blue;
            if (step) {
                channel = uint32_t(StepRange(int(channel), 0, 255, 1, step));
                o.crosshair_color = uint32_t(kEspPresetCount);
            }
            std::snprintf(text, sizeof(text), "%u", channel);
            return true;
        }
        case kAimbotTarget:
            if (step) o.aimbot_target = uint32_t(StepRange(int(o.aimbot_target), 0, 3, 1, step));
            std::snprintf(text, sizeof(text), "%s", kAimbotTargetNames[std::clamp(o.aimbot_target, 0u, 3u)]);
            return true;
        case kAimbotPull:
            if (step) o.aimbot_pull = uint32_t(StepRange(int(o.aimbot_pull), 1, 100, 1, step));
            std::snprintf(text, sizeof(text), "%u%%", o.aimbot_pull);
            return true;
        case kAimbotDeadzone:
            if (step) o.aimbot_deadzone = uint32_t(StepRange(int(o.aimbot_deadzone), 0, 300, 1, step));
            std::snprintf(text, sizeof(text), "%.1f", o.aimbot_deadzone / 10.0f);
            return true;
        case kGameSpeed:
            if (step) o.game_speed = uint32_t(StepRange(int(o.game_speed), 25, 200, 25, step));
            std::snprintf(text, sizeof(text), o.game_speed == 100 ? "%u%% (DEFAULT)" : "%u%%", o.game_speed);
            return true;
        case kCheatPanelSize:
            if (step) o.cheat_panel_size = uint32_t(StepRange(int(o.cheat_panel_size), 50, 300, 5, step));
            std::snprintf(text, sizeof(text), o.cheat_panel_size == 100 ? "%u%% (DEFAULT)" : "%u%%",
                          o.cheat_panel_size);
            return true;
        case kCheatButtonSize:
            if (step) o.cheat_button_size = uint32_t(StepRange(int(o.cheat_button_size), 50, 1000, 5, step));
            std::snprintf(text, sizeof(text), o.cheat_button_size == 100 ? "%u%% (DEFAULT)" : "%u%%",
                          o.cheat_button_size);
            return true;
        case kCheatButtonGap:
            if (step) o.cheat_button_gap = uint32_t(StepRange(int(o.cheat_button_gap), 0, 1000, 5, step));
            std::snprintf(text, sizeof(text), o.cheat_button_gap == 100 ? "%u%% (DEFAULT)" : "%u%%",
                          o.cheat_button_gap);
            return true;
        case kCheatReset:
            if (step) {
                o.cheat_panel_size = 100;
                o.cheat_button_size = 100;
                o.cheat_button_gap = 100;
            }
            std::snprintf(text, sizeof(text), "RESET ALL 3 -> 100%%");
            return true;
        default:
            return false;
    }
}

void StepInputRow(int id, int step, char (&text)[64]) {
    game::Options& o = game::Opt();
    switch (id) {
        case kAudioOutput:
            std::snprintf(text, sizeof(text), "%s", kAudioOutputNames[StepTable(kAudioOutputs, o.audio_output, step)]);
            break;
        case kSensitivity:
            if (step) {
                int tenths = StepRange(int(std::lround(o.mouse_sensitivity * 10.0f)), 1, 50, 1, step);
                o.mouse_sensitivity = float(tenths) / 10.0f;
            }
            std::snprintf(text, sizeof(text), "%.1f", o.mouse_sensitivity);
            break;
        case kTouchSensitivity:
            if (step) {
                int tenths = StepRange(int(std::lround(o.touch_sensitivity * 10.0f)), 1, 50, 1, step);
                o.touch_sensitivity = float(tenths) / 10.0f;
            }
            std::snprintf(text, sizeof(text), "%.1f", o.touch_sensitivity);
            break;
        case kWalkSpeed:
            if (step) o.walk_speed = uint32_t(StepRange(int(o.walk_speed), 10, 100, 5, step));
            std::snprintf(text, sizeof(text), "%u%%", o.walk_speed);
            break;
        case kGyro:
            if (step) gyro::SetMode(uint32_t(StepIndex(int(gyro::CurrentMode()), step, 3)));
            std::snprintf(text, sizeof(text), "%s", gyro::CurrentMode() == 0 ? "OFF"
                                                    : gyro::CurrentMode() == 1 ? "ALWAYS" : "WHILE FIRING");
            break;
        case kTouchLayout:
            if (step) touch::StepLayout(step);
            std::snprintf(text, sizeof(text), "%s", touch::LayoutName());
            break;
        case kTouchOpacity:
            if (step) touch::StepOpacity(step);
            std::snprintf(text, sizeof(text), "%u%%", touch::ControlsOpacity());
            break;
        case kTouchSize:
            if (step) touch::StepSize(step);
            std::snprintf(text, sizeof(text), "%u%%", touch::ControlsSize());
            break;
        case kTouchSkin:
            if (step) touch::StepSkin(step);
            std::snprintf(text, sizeof(text), "%s", touch::SkinName());
            break;
        case kTouchStick:
            if (step) touch::StepStickMode(step);
            std::snprintf(text, sizeof(text), "%s", touch::StickModeName());
            break;
        case kTouchEdit:
            if (step) touch::EditLayout();
            std::snprintf(text, sizeof(text), "OPEN");
            break;
        case kCheckUpdates:
            if (step) host::CheckForUpdates();
            std::snprintf(text, sizeof(text), "CHECK");
            break;
        case kGyroSensitivity:
            if (step) gyro::SetSensitivity(float(StepRange(int(std::lround(gyro::Sensitivity() * 10.0f)), 1, 50, 1, step)) / 10.0f);
            std::snprintf(text, sizeof(text), "%.1f", gyro::Sensitivity());
            break;
        case kGyroInvertX:
        case kGyroInvertY: {
            bool x = gyro::InvertX(), y = gyro::InvertY();
            if (step) (id == kGyroInvertX ? x : y) ^= true;
            if (step) gyro::SetInvert(x, y);
            std::snprintf(text, sizeof(text), "%s", OnOff(id == kGyroInvertX ? x : y));
            break;
        }
        case kLeftDeadzone:
        case kRightDeadzone: {
            uint32_t& dz = id == kLeftDeadzone ? o.left_deadzone : o.right_deadzone;
            if (step) dz = uint32_t(StepRange(int(dz), 0, 50, 1, step));
            std::snprintf(text, sizeof(text), dz ? "%u%%" : "%u%% (DEFAULT)", dz);
            break;
        }
        case kLeftSensitivity:
        case kRightSensitivity: {
            float& s = id == kLeftSensitivity ? o.left_sensitivity : o.right_sensitivity;
            if (step) s = float(StepRange(int(std::lround(s * 10.0f)), 1, 30, 1, step)) / 10.0f;
            std::snprintf(text, sizeof(text), "%.1f", s);
            break;
        }
        case kLanguage: {
            int count = int(std::size(game::kLanguages));
            int i = 0;
            while (i < count - 1 && o.language != game::kLanguages[i]) i++;
            if (step) {
                int before = i;
                do i = StepIndex(i, step, count);
                while (!game::LanguageAvailable(game::kLanguages[i]));
                if (i != before) {
                    std::string previous = o.language;
                    o.language = game::kLanguages[i];
                    // The game reads its language at boot, so any change to a code other than the one this
                    // process started with needs a fresh process. Ask the user; the modal offers Yes / No.
                    if (o.language != game::BootLanguage()) {
                        g_language_previous_code = previous;
                        g_language_pending_label = kLanguageNames[i];
                        g_language_restart_open = true;
                    }
                }
            }
            const char* format = o.language == game::BootLanguage() ? "%s" : "%s (RESTART)";
            std::snprintf(text, sizeof(text), format, kLanguageNames[i]);
            break;
        }
        case kAutoUpdate:
            if (step) host::SetStartupCheck(!host::StartupCheck());
            std::snprintf(text, sizeof(text), "%s", OnOff(host::StartupCheck()));
            break;
    }
}

std::string RowText(int id, int step, Settings& s = Get()) {
    char text[64] = "";
    if (bool* on = ToggleFlag(s, id)) {
        Flip(*on, step);
        std::snprintf(text, sizeof(text), "%s", OnOff(*on));
    } else if (!StepDisplayRow(id, step, s, text) && !StepGraphicsRow(id, step, s, text) &&
               !StepCheatRow(id, step, text)) {
        StepInputRow(id, step, text);
    }
    if (step && &s == &Get()) Changed();
    return text;
}

constexpr const char* kUnbound = "UNBOUND";

std::string KeyText(std::string keys) {
    if (keys.empty()) return kUnbound;
    for (char& c : keys) c = c == ',' ? '/' : char(std::toupper(uint8_t(c)));
    return keys;
}

constexpr int kTouchPage = 1;
constexpr int kGeneralPage = 2;
constexpr int kKeyboardPage = 3;
constexpr int kCheatsPage = 4;
constexpr int kCreditsPage = 5;
constexpr int kDebugPage = 6;

void RestoreDefaults(int page) {
    game::Options& o = game::Opt();
    if (page == kCheatsPage) {
        game::Options none;
        o.infinite_ammo = none.infinite_ammo;
        o.rapid_fire = none.rapid_fire;
        o.rapid_fire_extra = none.rapid_fire_extra;
        o.no_recoil = none.no_recoil;
        o.no_bullet_spread = none.no_bullet_spread;
        o.game_speed = none.game_speed;
        o.god_mode = none.god_mode;
        o.infinite_medkits = none.infinite_medkits;
        o.one_hit_kill = none.one_hit_kill;
        o.infinite_grenades = none.infinite_grenades;
        o.unlock_levels = none.unlock_levels;
        o.unlock_black_ops = none.unlock_black_ops;
        o.esp = none.esp;
        o.esp_display = none.esp_display;
        o.esp_color = none.esp_color;
        o.esp_red = none.esp_red;
        o.esp_green = none.esp_green;
        o.esp_blue = none.esp_blue;
        o.crosshair = none.crosshair;
        o.crosshair_style = none.crosshair_style;
        o.crosshair_size = none.crosshair_size;
        o.crosshair_thickness = none.crosshair_thickness;
        o.crosshair_gap = none.crosshair_gap;
        o.crosshair_color = none.crosshair_color;
        o.crosshair_red = none.crosshair_red;
        o.crosshair_green = none.crosshair_green;
        o.crosshair_blue = none.crosshair_blue;
        o.aimbot = none.aimbot;
        o.aimbot_target = none.aimbot_target;
        o.aimbot_pull = none.aimbot_pull;
        o.aimbot_deadzone = none.aimbot_deadzone;
        Changed();
        return;
    }
    if (page == kDebugPage) {
        game::Options none;
        o.cheat_panel_size = none.cheat_panel_size;
        o.cheat_button_size = none.cheat_button_size;
        o.cheat_button_gap = none.cheat_button_gap;
        Changed();
        return;
    }
    if (page == kKeyboardPage) {
        o.mouse_sensitivity = game::Options{}.mouse_sensitivity;
        o.invert_mouse = false;
        o.hold_aim = true;
        o.walk_speed = game::Options{}.walk_speed;
        input::ResetBindings();
        return;
    }
    game::Options keep = o;
    Get() = DefaultSettings();
    o = game::Options{};
    o.mouse_sensitivity = keep.mouse_sensitivity;
    o.invert_mouse = keep.invert_mouse;
    o.hold_aim = keep.hold_aim;
    o.walk_speed = keep.walk_speed;
    o.keys = keep.keys;
    o.infinite_ammo = keep.infinite_ammo;
    o.rapid_fire = keep.rapid_fire;
    o.rapid_fire_extra = keep.rapid_fire_extra;
    o.no_recoil = keep.no_recoil;
    o.no_bullet_spread = keep.no_bullet_spread;
    o.game_speed = keep.game_speed;
    o.god_mode = keep.god_mode;
    o.infinite_medkits = keep.infinite_medkits;
    o.one_hit_kill = keep.one_hit_kill;
    o.infinite_grenades = keep.infinite_grenades;
    o.unlock_levels = keep.unlock_levels;
    o.unlock_black_ops = keep.unlock_black_ops;
    o.esp = keep.esp;
    o.esp_display = keep.esp_display;
    o.esp_color = keep.esp_color;
    o.esp_red = keep.esp_red;
    o.esp_green = keep.esp_green;
    o.esp_blue = keep.esp_blue;
    o.crosshair = keep.crosshair;
    o.crosshair_style = keep.crosshair_style;
    o.crosshair_size = keep.crosshair_size;
    o.crosshair_thickness = keep.crosshair_thickness;
    o.crosshair_gap = keep.crosshair_gap;
    o.crosshair_color = keep.crosshair_color;
    o.crosshair_red = keep.crosshair_red;
    o.crosshair_green = keep.crosshair_green;
    o.crosshair_blue = keep.crosshair_blue;
    o.aimbot = keep.aimbot;
    o.aimbot_target = keep.aimbot_target;
    o.aimbot_pull = keep.aimbot_pull;
    o.aimbot_deadzone = keep.aimbot_deadzone;
    o.cheat_panel_size = keep.cheat_panel_size;
    o.cheat_button_size = keep.cheat_button_size;
    o.cheat_button_gap = keep.cheat_button_gap;
    Changed();
}

std::atomic<int> g_capture{-1};
int g_capture_slot = 0;
int g_hovered_box = -1;
bool g_capture_armed = false;
int g_capture_done = 0;
int g_page = kCheatsPage;

std::vector<std::string> KeyBoxes(size_t action) {
    std::string keys = input::Binding(action);
    size_t comma = keys.find(',');
    std::string second;
    if (comma != std::string::npos) second = keys.substr(comma + 1);
    second = second.substr(0, second.find(','));
    if (!second.empty()) second = KeyText(second);
    return {KeyText(keys.substr(0, comma)), second};
}

constexpr float kKeyBoxEms = 10.0f;

int BindRow(const char* label, const std::vector<std::string>& keys, int capturing, int& cleared) {
    const ImGuiStyle& style = ImGui::GetStyle();
    RowBox row = MeasureRow();
    float value_end = row.row_x + row.row_w - row.block_w + row.row_h + style.ItemSpacing.x + row.value_w;
    int n = int(keys.size());
    int pressed = -1;
    float box_w = n > 1 ? row.em * kKeyBoxEms : row.value_w;
    ImGui::PushID(label);
    RowLabel(row, label);
    for (int i = 0; i < n; i++) {
        float x = value_end - float(n - i) * box_w - float(n - 1 - i) * style.ItemSpacing.x;
        ImGui::SetCursorPos(ImVec2(x, row.row_y));
        ImGui::PushID(i);
        if (ImGui::InvisibleButton("##key", ImVec2(box_w, row.row_h))) pressed = i;
        bool deleted = ImGui::IsItemFocused() && ImGui::IsKeyPressed(ImGuiKey_Delete, false);
        if (ImGui::IsItemClicked(ImGuiMouseButton_Right) || deleted) cleared = i;
        bool pointed = ImGui::IsItemHovered() || ImGui::IsItemFocused();
        if (pointed && !keys[i].empty() && keys[i] != kUnbound) g_hovered_box = i;
        bool lit = capturing == i || ImGui::IsItemFocused() || ImGui::IsItemHovered();
        ImGui::GetWindowDrawList()->AddRectFilled(ImGui::GetItemRectMin(), ImGui::GetItemRectMax(),
                                                  ImGui::GetColorU32(lit ? ImGuiCol_Header : ImGuiCol_FrameBg));
        std::string value = keys[i];
        if (capturing == i) value = Ui("PRESS A KEY");
        else if (value == kUnbound) value = Ui(kUnbound);
        float pad = (box_w - ImGui::CalcTextSize(value.c_str()).x) * 0.5f;
        ImGui::SetCursorPos(ImVec2(x + std::max(pad, 0.0f), row.row_y));
        ImGui::AlignTextToFramePadding();
        if (capturing == i) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
        ImGui::TextUnformatted(value.c_str());
        if (capturing == i) ImGui::PopStyleColor();
        ImGui::PopID();
    }
    ImGui::PopID();
    EndRow(row);
    return pressed;
}

void PollCapture() {
    if (g_capture < 0) return;
    int vk = input::PressedKey();
    if (!g_capture_armed) {
        g_capture_armed = !vk;
        return;
    }
    if (!vk) return;
    if (vk != VK_ESCAPE) input::Bind(size_t(g_capture.load()), vk, g_capture_slot);
    g_capture = -1;
    g_capture_done = ImGui::GetFrameCount();
}

bool CaptureFree() { return g_capture < 0 && ImGui::GetFrameCount() > g_capture_done + 1; }

void ValueRow(const Row& r) {
    int step = OptionRow(r.label, Ui(RowText(r.id, 0)).c_str());
    if (step) RowText(r.id, step);
}

void KeyRow(const Row& r) {
    int cleared = -1;
    int box = BindRow(r.label, KeyBoxes(size_t(r.id)), g_capture == r.id ? g_capture_slot : -1, cleared);
    if (cleared >= 0 && CaptureFree()) input::Bind(size_t(r.id), 0, cleared);
    if (box < 0 || !CaptureFree()) return;
    g_capture_slot = box;
    g_capture = r.id;
    g_capture_armed = false;
}

void DrawList(const Row* list, size_t count) {
    bool expanded = true;
    for (size_t i = 0; i < count; i++) {
        const Row& r = list[i];
        if (r.kind == Kind::Heading) {
            expanded = Section(r.label);
        } else if (expanded) {
            if (r.kind == Kind::Value) ValueRow(r);
            else KeyRow(r);
        }
    }
}

void DrawRows(int page) {
    size_t count = 0;
    const Row* list = rows::Page(page, &count);
    DrawList(list, count);
    if (page != kTouchPage) return;
    if (Section("CONTROLLER")) DrawList(kControllerRows, std::size(kControllerRows));
}

void DrawWeapons() {
    weapons::View v = weapons::Current();
    if (Section("GIVE A WEAPON")) {
        if (!v.in_level) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::TextWrapped("%s", Ui("Start or continue a level to choose its weapons.", false).c_str());
            ImGui::PopStyleColor();
        } else {
            constexpr int kColumns = 3;
            float gap = ImGui::GetStyle().ItemSpacing.x;
            float w = (ImGui::GetContentRegionAvail().x - gap * (kColumns - 1)) / kColumns;
            struct GroupTitle {
                const char* name;
                const char* hint;
            };
            static const GroupTitle kGroupTitles[] = {{"SUPPORTED SUPPRESSOR", "PRESS Y TO TAKE IT OFF"},
                                                      {"UNSUPPORTED SUPPRESSOR", "EXPERIMENTAL"}};
            auto sub_title = [](const char* text) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted(Ui(text).c_str());
                ImGui::PopStyleColor();
            };
            bool sound_only = false;
            int group = -1, column = 0, variant = -1;
            for (size_t i = 0; i < v.choices.size(); i++) {
                const weapons::Choice& c = v.choices[i];
                if (c.group == group && variant != ((c.type & 0x100) ? 1 : 0)) {
                    variant = (c.type & 0x100) ? 1 : 0;
                    column = 0;
                    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.15f));
                    if (group == 1) sub_title("WITH SUPPRESSOR (PERMANENT)");
                }
                if (c.group != group) {
                    group = c.group;
                    variant = (c.type & 0x100) ? 1 : 0;
                    column = 0;
                    const GroupTitle& title = kGroupTitles[std::clamp(group, 0, 1)];
                    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.25f));
                    ImGui::TextUnformatted(Ui(title.name).c_str());
                    ImGui::SameLine();
                    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                    ImGui::TextUnformatted(Ui(title.hint).c_str());
                    ImGui::PopStyleColor();
                    ImGui::Dummy(ImVec2(0, ImGui::GetFontSize() * 0.05f));
                    if (group == 1) sub_title("NO SUPPRESSOR");
                }
                if (column++ % kColumns) ImGui::SameLine();
                ImGui::PushID(c.type);
                if (c.held) ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
                if (ImGui::Button(c.label.c_str(), ImVec2(w, 0))) weapons::Give(c.type);
                if (c.held) ImGui::PopStyleColor();
                ImGui::PopID();
                sound_only = sound_only || c.sound_only;
            }
            if (sound_only) {
                ImGui::PushStyleColor(ImGuiCol_Text, kDim);
                ImGui::TextUnformatted(Ui("* SILENCED SOUND ONLY").c_str());
                ImGui::PopStyleColor();
            }
        }
    }
    if (Section("SUPPLIES")) {
        if (ImGui::Button(Ui("FILL AMMUNITION").c_str(), ImVec2(-1, 0))) weapons::FillAmmo();
        if (ImGui::Button(Ui("FILL MEDKITS").c_str(), ImVec2(-1, 0))) weapons::FillMedkits();
        if (ImGui::Button(Ui("FILL GRENADES").c_str(), ImVec2(-1, 0))) weapons::FillGrenades();
        if (!v.status.empty()) {
            ImGui::PushStyleColor(ImGuiCol_Text, kDim);
            ImGui::AlignTextToFramePadding();
            ImGui::TextUnformatted(v.status.c_str());
            ImGui::PopStyleColor();
        }
    }
}

// The confirmation shown after the user steps LANGUAGE to a code other than the boot language. The
// row's "(RESTART)" suffix tells the user a restart is pending; this modal gives them the choice.
//   Yes (Restart) — Save() is a no-op if nothing else changed, then SDL_EVENT_QUIT lets the main loop
//                    quit cleanly. On Android the activity finishes, the launcher can relaunch; on
//                    desktop the process ends and a relaunch picks the new language up at boot.
//   No (Cancel)  — revert to g_language_previous_code and Save() again, so a relaunch would boot
//                    with the old language (the change never took effect in the running game either).
//   Escape       — same as Cancel.
// Closing the panel while the modal is open also cancels; see ToggleMenu().
void DrawLanguageRestartDialog(float em) {
    if (g_language_restart_open && !ImGui::IsPopupOpen("##language_restart")) {
        ImGui::OpenPopup("##language_restart");
    }
    if (!ImGui::BeginPopupModal("##language_restart", nullptr,
                                ImGuiWindowFlags_AlwaysAutoResize | ImGuiWindowFlags_NoMove)) {
        return;
    }

    ImGui::PushStyleColor(ImGuiCol_Text, kHeading);
    ImGui::TextUnformatted(Ui("RESTART REQUIRED").c_str());
    ImGui::PopStyleColor();
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    ImGui::TextWrapped("%s", Ui("The game must restart to apply the new language.").c_str());
    ImGui::Spacing();
    {
        char line[128];
        std::snprintf(line, sizeof(line), "%s: %s", Ui("NEW LANGUAGE").c_str(), g_language_pending_label.c_str());
        ImGui::TextUnformatted(line);
    }
    ImGui::Spacing();
    ImGui::Separator();
    ImGui::Spacing();

    float bw = em * 11.0f;
    float total = bw * 2.0f + ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(std::max((ImGui::GetWindowWidth() - total) * 0.5f, 0.0f));

    auto cancel = [] {
        g_language_restart_open = false;
        game::Opt().language = g_language_previous_code;
        Changed();
    };

    if (ImGui::Button(Ui("Yes (Restart)").c_str(), ImVec2(bw, 0))) {
        g_language_restart_open = false;
        ImGui::CloseCurrentPopup();
        RestartForLanguage();
    }
    ImGui::SameLine();
    if (ImGui::Button(Ui("No (Cancel)").c_str(), ImVec2(bw, 0))) {
        cancel();
        ImGui::CloseCurrentPopup();
    }
    if (ImGui::IsKeyPressed(ImGuiKey_Escape)) {
        cancel();
        ImGui::CloseCurrentPopup();
    }
    ImGui::EndPopup();
}

void Defaults() {
    g_capture = -1;
    RestoreDefaults(g_page);
    game::Options& o = game::Opt();
    o.panel_pos_x = INT32_MIN;
    o.panel_pos_y = INT32_MIN;
}

void DrawTitle(const char* caption) {
    std::string title = Ui(caption);
    float title_pad = (ImGui::GetContentRegionAvail().x - ImGui::CalcTextSize(title.c_str()).x) * 0.5f;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(title_pad, 0.0f));
    ImGui::TextUnformatted(title.c_str());
}

void DrawTabs() {
    struct Tab { const char* label; int page; };
    static const Tab kTabs[] = {
        {"GRAPHICS", 0},
        {"TOUCH",    1},
        {"GENERAL",  2},
        {"KEYBOARD", 3},
        {"CHEATS",   4},
        {"DEBUG",    6},
        {"CREDITS",  5},
    };
    float em = ImGui::GetFontSize();
    const ImGuiStyle& style = ImGui::GetStyle();
    int n = int(std::size(kTabs));
    float gap = em * 0.25f;
    float total_w = ImGui::GetContentRegionAvail().x;
    float tab_w = (total_w - gap * (n - 1)) / n;
    float tab_h = ImGui::GetFrameHeight();
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(gap, style.ItemSpacing.y));
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, em * 0.3f);
    for (int i = 0; i < n; i++) {
        if (i) ImGui::SameLine();
        bool active = (g_page == kTabs[i].page);
        if (active) {
            ImGui::PushStyleColor(ImGuiCol_Button, kHeading);
            ImGui::PushStyleColor(ImGuiCol_ButtonHovered, kHeading);
            ImGui::PushStyleColor(ImGuiCol_ButtonActive, kHeading);
            ImGui::PushStyleColor(ImGuiCol_Text, kPanelBg);
        }
        if (ImGui::Button(kTabs[i].label, ImVec2(tab_w, tab_h))) {
            g_page = kTabs[i].page;
            g_capture = -1;
        }
        if (active) ImGui::PopStyleColor(4);
    }
    ImGui::PopStyleVar(2);
    ImGui::Separator();
}

void DrawFooter(float em) {
    ImGui::Separator();
    ImGui::PushStyleColor(ImGuiCol_Text, kDim);
    const char* hint = "";
    if (g_capture >= 0) hint = "Press a key or mouse button to bind, Escape cancels";
    else if (g_page == kKeyboardPage && g_hovered_box >= 0) hint = "Right click or Delete clears the key";
    else if (g_page == kCheatsPage) hint = "Drag anywhere to move the panel";
    ImGui::TextUnformatted(Ui(hint, false).c_str());
    ImGui::PopStyleColor();
    float bw = em * 8;
    float buttons_pad = ImGui::GetContentRegionAvail().x - bw * 2 - ImGui::GetStyle().ItemSpacing.x;
    ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(buttons_pad, 0.0f));
    if (ImGui::Button(Ui("Back", false).c_str(), ImVec2(bw, 0))) ToggleMenu();
    ImGui::SameLine();
    if (ImGui::Button(Ui("Defaults", false).c_str(), ImVec2(bw, 0))) Defaults();
}

void DragScroll(float em) {
    constexpr float kFriction = 6.0f;
    static bool dragging = false;
    static float glide = 0;
    ImGuiIO& io = ImGui::GetIO();
    bool inside = ImGui::GetCurrentWindow()->InnerRect.Contains(io.MouseClickedPos[0]);
    if (ImGui::IsMouseClicked(0)) {
        dragging = false;
        glide = 0;
    }
    if (ImGui::IsMouseDown(0) && inside) {
        if (!dragging && std::fabs(ImGui::GetMouseDragDelta(0).y) > em * 0.5f) dragging = true;
        if (!dragging) return;
        ImGui::SetScrollY(ImGui::GetScrollY() - io.MouseDelta.y);
        if (io.DeltaTime > 0) glide = (glide - io.MouseDelta.y / io.DeltaTime) * 0.5f;
        ImGui::ClearActiveID();
        return;
    }
    dragging = false;
    if (std::fabs(glide) < em) {
        glide = 0;
        return;
    }
    ImGui::SetScrollY(ImGui::GetScrollY() + glide * io.DeltaTime);
    glide *= std::max(0.0f, 1.0f - kFriction * io.DeltaTime);
}

constexpr float kUiHeightShare = 0.030f;
constexpr float kUiWidthShare = 0.0192f;

void DrawPanel() {
    const ImGuiViewport* vp = ImGui::GetMainViewport();
    float em = ImGui::GetFontSize();
    ImVec2 win_size(em * 80, em * 50);
    ImVec2 center = vp->GetCenter();
    ImVec2 pos(center.x - win_size.x * 0.5f, center.y - win_size.y * 0.5f);
    if (game::Opt().panel_pos_x != INT32_MIN && game::Opt().panel_pos_y != INT32_MIN) {
        pos = ImVec2(float(game::Opt().panel_pos_x), float(game::Opt().panel_pos_y));
    }
    ImGui::SetNextWindowPos(pos, ImGuiCond_Always, ImVec2(0.0f, 0.0f));
    ImGui::SetNextWindowSize(win_size, ImGuiCond_Always);
    for (const auto& [col, color] : kPanelColors) ImGui::PushStyleColor(col, color);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowBorderSize, 2.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    ImGui::PushStyleVar(ImGuiStyleVar_WindowPadding, ImVec2(em * 1.6f, em * 0.9f));
    ImGui::PushStyleVar(ImGuiStyleVar_FramePadding, ImVec2(em * 0.7f, em * 0.45f));
    ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing, ImVec2(em * 0.6f, em * 0.25f));
    constexpr ImGuiWindowFlags kFlags = ImGuiWindowFlags_NoResize | ImGuiWindowFlags_NoCollapse |
                                        ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_NoTitleBar |
                                        ImGuiWindowFlags_NoMove;
    if (ImGui::Begin("##settings", nullptr, kFlags)) {
        DrawTitle("MENU");
        ImGui::Separator();
        DrawTabs();
        float footer = ImGui::GetTextLineHeightWithSpacing() + ImGui::GetFrameHeightWithSpacing() + em;
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarSize, em * 1.8f);
        ImGui::PushStyleVar(ImGuiStyleVar_ScrollbarRounding, 0.0f);
        ImGui::PushStyleVar(ImGuiStyleVar_GrabMinSize, em * 2.5f);
        ImGui::BeginChild("##rows", ImVec2(0, -footer), ImGuiChildFlags_NavFlattened);
        DragScroll(em);
        g_hovered_box = -1;
        DrawRows(g_page);
        if (g_page == kCheatsPage) DrawWeapons();
        ImGui::EndChild();
        ImGui::PopStyleVar(3);
        DrawFooter(em);

        if (!g_panel_dragging
            && ImGui::IsMouseClicked(ImGuiMouseButton_Left)
            && ImGui::IsWindowHovered()
            && !ImGui::IsAnyItemActive()) {
            g_panel_dragging = true;
            g_panel_drag_origin = ImGui::GetWindowPos();
            g_panel_mouse_origin = ImGui::GetIO().MousePos;
            g_panel_drag_moved = false;
        }
        if (g_panel_dragging) {
            if (!ImGui::IsMouseDown(ImGuiMouseButton_Left)) {
                g_panel_dragging = false;
                if (g_panel_drag_moved) Changed();
            } else {
                ImVec2 mouse = ImGui::GetIO().MousePos;
                ImVec2 d(mouse.x - g_panel_mouse_origin.x, mouse.y - g_panel_mouse_origin.y);
                if (d.x != 0 || d.y != 0) g_panel_drag_moved = true;
                ImVec2 np(g_panel_drag_origin.x + d.x, g_panel_drag_origin.y + d.y);
                float min_vis = em * 4;
                ImVec2 ws = ImGui::GetWindowSize();
                np.x = std::clamp(np.x, -ws.x + min_vis, vp->WorkSize.x - min_vis);
                np.y = std::clamp(np.y, -ws.y + min_vis, vp->WorkSize.y - min_vis);
                ImGui::SetWindowPos(np);
                game::Options& o = game::Opt();
                o.panel_pos_x = int32_t(np.x);
                o.panel_pos_y = int32_t(np.y);
            }
        }

        DrawLanguageRestartDialog(em);
    }
    ImGui::End();
    ImGui::PopStyleVar(6);
    ImGui::PopStyleColor(int(std::size(kPanelColors)));
}

uint32_t RefreshRate() {
    SDL_Window* w = static_cast<SDL_Window*>(Window());
    const SDL_DisplayMode* m = SDL_GetCurrentDisplayMode(w ? SDL_GetDisplayForWindow(w) : SDL_GetPrimaryDisplay());
    return m && m->refresh_rate > 1 ? uint32_t(std::lround(m->refresh_rate)) : 0;
}

void PaceAt(uint32_t cap) {
    using clock = std::chrono::steady_clock;
    static clock::time_point next = clock::now();
    auto now = clock::now();
    if (!cap) {
        next = now;
        return;
    }
    auto period = std::chrono::nanoseconds(1000000000ull / cap);
    next += period;
    if (next < now - period) next = now;
    while (clock::now() + std::chrono::milliseconds(2) < next) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    while (clock::now() < next) std::this_thread::yield();
}

constexpr float kPanelWideEms = 80.0f;
constexpr float kPanelTallEms = 50.0f;
constexpr float kPanelMargin = 1.08f;

float InterfaceScale(ImGuiIO& io) {
    float font_base = io.FontGlobalScale > 0.0f ? ImGui::GetFontSize() / io.FontGlobalScale : 16.0f;
    float fit_x = io.DisplaySize.x / (kPanelWideEms * font_base * kPanelMargin);
    float fit_y = io.DisplaySize.y / (kPanelTallEms * font_base * kPanelMargin);
    float density = std::min(io.DisplaySize.x, io.DisplaySize.y) / 720.0f;
    float base = std::min({density, fit_x, fit_y});
    float user = float(std::clamp(game::Opt().cheat_panel_size, 50u, 300u)) / 100.0f;
    return std::clamp(base * user, 0.5f, 4.0f);
}

}  // namespace

void PaceFrame() {
    if (Get().fps_cap != kFpsCapRefresh) {
        Pace();
        return;
    }
    constexpr uint32_t kRefreshMargin = 3;
    uint32_t rate = RefreshRate();
    PaceAt(rate > kRefreshMargin ? rate - kRefreshMargin : 0);
}

const rows::Row* rows::Page(int page, size_t* count) {
    if (page == kKeyboardPage) { *count = InputRows().size(); return InputRows().data(); }
    if (page == kCheatsPage) { *count = std::size(kCheatRows); return kCheatRows; }
    if (page == kDebugPage) { *count = std::size(kDebugRows); return kDebugRows; }
    if (page == kCreditsPage) { *count = CreditRows().size(); return CreditRows().data(); }
    if (page == kTouchPage) { *count = std::size(kTouchRows); return kTouchRows; }
    if (page == kGeneralPage) { *count = std::size(kGeneralRows); return kGeneralRows; }
    *count = std::size(kGraphicsRows);
    return kGraphicsRows;
}

const rows::Row* rows::Controller(size_t* count) {
    *count = std::size(kControllerRows);
    return kControllerRows;
}

std::string rows::Value(const Row& row, int step) {
    if (row.kind == Kind::Bind) return KeyText(input::Binding(size_t(row.id)));
    if (row.id == kCredit) {
        const Credit* c = CreditOf(row);
        if (!c || !c->value) return " ";
        if (step && c->url) SDL_OpenURL(c->url);
        return c->value;
    }
    return RowText(row.id, step);
}

void rows::Defaults(int page) { RestoreDefaults(page); }
bool rows::Restorable(int page) { return page == kKeyboardPage || page == kCheatsPage || page == kDebugPage; }

bool rows::Enabled(const Row& row) {
    if (row.kind != Kind::Value) return true;
    bool dynamic = game::Opt().dynamic_resolution;
    switch (row.id) {
        case kRenderResolution: return !dynamic;
        case kDynamicTarget:
        case kDynamicMin:
        case kDynamicMax:
        case kDynamicHold: return dynamic;
        default: return true;
    }
}

bool rows::Opens(const Row& row) {
    return row.kind == Kind::Value && (row.id == kTouchEdit || row.id == kCheckUpdates || row.id == kCredit);
}

bool MenuOpen() { return g_open.load(std::memory_order_relaxed); }
bool Capturing() { return g_capture >= 0; }

void ToggleMenu() {
    bool open = !MenuOpen();
    g_open.store(open, std::memory_order_relaxed);
    g_capture = -1;
    g_panel_dragging = false;
    // Closing the panel while the language-restart modal is open is treated as Cancel: the new
    // language never applied to this process, and leaving "(RESTART)" on the row would be misleading.
    // Reopen the panel and the LANGUAGE row is back on the language the process actually booted with.
    if (!open && g_language_restart_open) {
        g_language_restart_open = false;
        game::Opt().language = g_language_previous_code;
        Changed();
    }
    if (open) input::ReleaseCursor();
}

void DrawUi(double fps) {
    game::g_fps = float(fps);
    ImGuiIO& io = ImGui::GetIO();
    if (MenuOpen()) io.ConfigFlags &= ~ImGuiConfigFlags_NoMouseCursorChange;
    else io.ConfigFlags |= ImGuiConfigFlags_NoMouseCursorChange;
    io.KeyRepeatRate = 0.1f;
    io.FontGlobalScale = InterfaceScale(io);
    if (MenuOpen()) {
        PollCapture();
        DrawPanel();
    }
}

}  // namespace app
