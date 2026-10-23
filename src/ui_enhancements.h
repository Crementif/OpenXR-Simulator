// UI Enhancements for OpenXR Simulator
// Provides dark theme, menu system, and view controls
#pragma once
#include "simulation/HeadsetProfiles.h"

#include <windows.h>
#include <cstdint>
#include <mutex>
#include <atomic>
#include <dwmapi.h>
#include <string>
#include <functional>
#include <filesystem>
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <cstdlib>

#include "json.h"

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "uxtheme.lib")

namespace ui {

// Menu command IDs
enum MenuCommand {
    // View Menu
    ID_VIEW_BOTH_EYES = 1001,
    ID_VIEW_LEFT_EYE = 1002,
    ID_VIEW_RIGHT_EYE = 1003,

    // Zoom levels
    ID_ZOOM_25 = 1101,
    ID_ZOOM_50 = 1102,
    ID_ZOOM_75 = 1103,
    ID_ZOOM_100 = 1104,
    ID_ZOOM_FIT = 1105,
    ID_ZOOM_IN = 1106,
    ID_ZOOM_OUT = 1107,

    // FOV options
    ID_FOV_70 = 1201,
    ID_FOV_90 = 1202,
    ID_FOV_110 = 1203,
    ID_FOV_SYMMETRIC = 1204,
    ID_FOV_ASYMMETRIC = 1205,
    ID_IPD_0 = 1220,
    ID_IPD_58 = 1221,
    ID_IPD_64 = 1222,
    ID_IPD_70 = 1223,
    ID_IPD_80 = 1224,
    ID_IPD_DECREASE = 1225,
    ID_IPD_INCREASE = 1226,

    // Render options
    ID_VIEW_FULL_RENDER = 1250,

    // Display options
    ID_DISPLAY_SIDE_BY_SIDE = 1301,
    ID_DISPLAY_OVER_UNDER = 1302,
    ID_DISPLAY_ANAGLYPH = 1303,

    // Tools
    ID_TOOLS_SCREENSHOT = 1401,
    ID_TOOLS_RESET_VIEW = 1402,
    ID_TOOLS_TOGGLE_STATS = 1403,

    // Help
    ID_HELP_CONTROLS = 1501,
    ID_HELP_ABOUT = 1502,

    // One id per kHeadsetSpecs entry, in table order.
    ID_PROFILE_FIRST = 1600,
    ID_PROFILE_LAST = 1663,

    // Movement speed. The two blocks hold one id per kMoveSpeedPresets /
    // kMoveBoostPresets entry, in table order.
    ID_MOVE_SLOWER = 1701,
    ID_MOVE_FASTER = 1702,
    ID_MOVE_SPEED_FIRST = 1710,
    ID_MOVE_SPEED_LAST = 1725,
    ID_MOVE_BOOST_FIRST = 1730,
    ID_MOVE_BOOST_LAST = 1745,

    // One id per kPreviewRatePresets entry, in table order.
    ID_PREVIEW_RATE_FIRST = 1750,
    ID_PREVIEW_RATE_LAST = 1765,

    // One id per kRenderResolutionPresets entry, in table order.
    ID_RENDER_RESOLUTION_FIRST = 1770,
    ID_RENDER_RESOLUTION_LAST = 1799,

    // One id per kInputPollingPresets entry, in table order.
    ID_INPUT_POLLING_FIRST = 1800,
    ID_INPUT_POLLING_LAST = 1809
};

// View mode enum
enum class ViewMode {
    BothEyes,
    LeftEyeOnly,
    RightEyeOnly
};

// Display layout enum
enum class DisplayLayout {
    SideBySide,
    OverUnder,
    Anaglyph
};

// Headset profile enum. Values index kHeadsetSpecs, so the two must stay in
// the same order.
enum class HeadsetProfile {
    GenericSymmetric,
    Quest2,
    Quest3,
    QuestPro,
    ValveIndex,
    VivePro2,
    ReverbG2,
    PSVR2,
    Pico4,
    BigscreenBeyond
};

// Values index kInputPollingPresets, so the two must stay in the same order.
enum class InputPolling {
    PreviewFocused,
    AppForeground,
    Background
};

// UI State
struct UIState {
    ViewMode viewMode = ViewMode::BothEyes;
    DisplayLayout displayLayout = DisplayLayout::SideBySide;
    HeadsetProfile headsetProfile = HeadsetProfile::Quest3;
    bool showStats = false;

    // Recommended per-eye render size reported to the application. This is deliberately
    // independent of headset geometry: FOV/IPD can emulate Quest 3 while a lower render
    // resolution keeps a desktop simulation fast. 0x0 means the active headset's native
    // panel size. 1280x1400 matches the simulator's pre-native-panel performance posture.
    int renderWidth = 1280;
    int renderHeight = 1400;

    // Zoom scales the image inside the window; it never changes the application's render
    // resolution. zoomLevel is an absolute content scale (1.0 = one panel pixel per screen
    // pixel) and pan offsets the scaled image from centred, in client pixels. With
    // fitToWindow set, the compositor scales the image up or down until it just fits the
    // client area, so a low or supersampled source occupies the same screen space. The
    // scale stays uniform and the leftover client area is left black: a window the user
    // dragged to some other shape must not be reported back as a squeezed render.
    float zoomLevel = 1.0f;
    bool fitToWindow = true;
    float panX = 0.0f;
    float panY = 0.0f;

    // Client size the preview window was last left at. 0 until the window has existed
    // once, which is what makes the first run fall back to the panel's shape.
    int windowWidth = 0;
    int windowHeight = 0;

    // FOV settings
    int fovDegrees = 90;     // FOV in degrees for generic symmetric mode
    bool useAsymmetricFov = true;
    float ipdMeters = 0.064f;

    // Render options
    bool showFullRender = false;  // If true, show full swapchain instead of imageRect crop

    // Head movement (WASD/QE) in meters per second, and what holding Shift
    // multiplies it by.
    float moveSpeed = 3.0f;
    float moveBoost = 4.0f;

    // How often the mirror window is allowed to update, in Hz. Mirroring is not free -
    // it composites the eyes, reads them back and repaints the window - and none of that
    // buys anything above the rate a monitor shows. 0 freezes the mirror (the app still
    // runs normally); kPreviewRateEveryFrame follows the app.
    int previewFps = 90;

    // What the polling gate in xrWaitFrame admits.
    InputPolling inputPolling = InputPolling::AppForeground;
};

// Value helpers nest. Protect settings only; release before window callbacks,
// file I/O or graphics work.
inline std::recursive_mutex g_stateMutex;
inline UIState g_uiState;
inline UIState ReadState() {
    std::lock_guard lock(g_stateMutex);
    return g_uiState;
}

// Everything that varies per headset, in one table.
//
// panelWidth/panelHeight is the native per-eye panel resolution: the shape the preview
// uses for headset geometry and the value returned by the "Headset Native" render preset.
// It is intentionally separate from the configurable render recommendation -- see
// "Why the preview used to look stretched" in BETTERVR.md.
//
// The frustum half-angles are in degrees, following the XrFovf sign convention. Both
// eyes are transcribed from the HMD Geometry Database
// (https://risa2000.github.io/hmdgdb/), which records what each headset's runtime
// actually reports. Real hardware is not exactly mirrored, so each eye carries its own
// measured values rather than one being derived from the other.
using EyeFov = simxr::HeadsetEyeFov;
using HeadsetSpec = simxr::HeadsetSpec;
inline constexpr auto& kHeadsetSpecs = simxr::HeadsetProfileTable;

inline constexpr int kHeadsetProfileCount =
    (int)(sizeof(kHeadsetSpecs) / sizeof(kHeadsetSpecs[0]));

static_assert(kHeadsetProfileCount <= ID_PROFILE_LAST - ID_PROFILE_FIRST + 1,
              "kHeadsetSpecs outgrew the reserved menu id block");

inline const HeadsetSpec& GetHeadsetSpec(HeadsetProfile profile) {
    int i = (int)profile;
    if (i < 0 || i >= kHeadsetProfileCount) i = 0;
    return kHeadsetSpecs[i];
}

inline const HeadsetSpec& GetActiveHeadsetSpec() {
    std::lock_guard stateLock(g_stateMutex);
    return GetHeadsetSpec(g_uiState.headsetProfile);
}

// Whether the reported frustum is the profile's measured geometry rather than the
// symmetric fallback. The generic profile has no geometry of its own, so it stays
// symmetric even if a stale settings file asks for asymmetry.
inline bool UsesProfileFov() {
    std::lock_guard stateLock(g_stateMutex);
    return g_uiState.useAsymmetricFov &&
           g_uiState.headsetProfile != HeadsetProfile::GenericSymmetric;
}

// Aspect of the reported frustum in tangent space -- the shape of the rectangle it cuts
// out of a plane, which is what a render target has to match. Both eyes give the same
// answer: real geometry is asymmetric per eye but mirrored between them.
inline float ActiveFovAspect() {
    std::lock_guard stateLock(g_stateMutex);
    if (!UsesProfileFov()) return 1.0f;   // equal half-angles all round
    const EyeFov& e = GetActiveHeadsetSpec().eye[0];
    const float DEG2RAD = 3.14159265f / 180.0f;
    const float w = tanf(e.angleRight * DEG2RAD) - tanf(e.angleLeft * DEG2RAD);
    const float h = tanf(e.angleUp * DEG2RAD) - tanf(e.angleDown * DEG2RAD);
    return (w > 0.0f && h > 0.0f) ? w / h : 1.0f;
}

// Index into kHeadsetSpecs, or -1 when `s` names no known profile.
inline int FindHeadsetSpec(const char* s) {
    for (int i = 0; i < kHeadsetProfileCount; ++i) {
        if (strcmp(s, kHeadsetSpecs[i].id) == 0) return i;
    }
    return -1;
}

struct RenderResolutionPreset {
    int width;
    int height;
    const wchar_t* label;
};

// These are recommendations, not preview-window sizes. Applications may round them for
// alignment (BetterVR rounds 2064 to 2080, for example), but choosing a lower entry still
// directly reduces the render target and depth-buffer cost in applications that follow it.
// The 0x0 entry follows the active headset profile. Only the width of an entry is
// advertised as written -- GetRenderResolution derives the height from the reported FOV.
inline constexpr RenderResolutionPreset kRenderResolutionPresets[] = {
    {    0,    0, L"&Headset Native" },
    {  960, 1080, L"&960 x 1080 (Low)" },
    { 1280, 1400, L"&1280 x 1400 (Performance)" },
    { 1440, 1584, L"&1440 x 1584 (Balanced)" },
    { 1832, 1920, L"&1832 x 1920 (High)" },
    { 2064, 2208, L"&2064 x 2208" },
    { 2560, 2560, L"&2560 x 2560 (Supersample)" },
};

inline constexpr int kRenderResolutionPresetCount =
    (int)(sizeof(kRenderResolutionPresets) / sizeof(kRenderResolutionPresets[0]));

static_assert(kRenderResolutionPresetCount <=
              ID_RENDER_RESOLUTION_LAST - ID_RENDER_RESOLUTION_FIRST + 1,
              "kRenderResolutionPresets outgrew the reserved menu id block");

inline bool IsRenderResolutionCommand(int cmd) {
    return cmd >= ID_RENDER_RESOLUTION_FIRST &&
           cmd < ID_RENDER_RESOLUTION_FIRST + kRenderResolutionPresetCount;
}

inline constexpr int kMinRenderWidth = 320;
inline constexpr int kMinRenderHeight = 240;
// Also what xrEnumerateViewConfigurationViews reports as maxImageRect, so nothing we
// recommend can land outside what we told the application it may ask for.
inline constexpr int kMaxRenderExtent = 4096;

inline void SetRenderResolution(int width, int height) {
    std::lock_guard stateLock(g_stateMutex);
    if (width <= 0 || height <= 0) {
        g_uiState.renderWidth = 0;
        g_uiState.renderHeight = 0;
        return;
    }
    g_uiState.renderWidth = (std::max)(kMinRenderWidth, (std::min)(kMaxRenderExtent, width));
    g_uiState.renderHeight = (std::max)(kMinRenderHeight, (std::min)(kMaxRenderExtent, height));
}

// What xrEnumerateViewConfigurationViews advertises. A preset is a pixel budget, not a
// shape: an application renders the frustum we report into the target we recommend, so
// the two have to agree on aspect or everything it draws comes out anamorphic -- and
// nothing downstream can tell that from a deliberate squeeze. Real optics never disagree
// here because both numbers describe one lens; ours are independent, so the chosen width
// stands and the height follows the frustum.
inline void GetRenderResolution(uint32_t& width, uint32_t& height) {
    std::lock_guard stateLock(g_stateMutex);
    if (g_uiState.renderWidth > 0 && g_uiState.renderHeight > 0) {
        width = (uint32_t)g_uiState.renderWidth;
        height = (uint32_t)g_uiState.renderHeight;
    } else {
        const HeadsetSpec& spec = GetActiveHeadsetSpec();
        width = spec.panelWidth;
        height = spec.panelHeight;
    }

    const float aspect = ActiveFovAspect();
    if (width == 0 || !(aspect > 0.0f)) return;

    long snapped = lroundf((float)width / aspect);
    if (snapped > kMaxRenderExtent) {
        // Losing some width beats recommending a target past our own maxImageRect.
        width = (uint32_t)lroundf((float)kMaxRenderExtent * aspect);
        snapped = kMaxRenderExtent;
    }
    height = (uint32_t)(std::max)(1L, snapped);
}

inline int GetRenderResolutionPresetIndex() {
    std::lock_guard stateLock(g_stateMutex);
    for (int i = 0; i < kRenderResolutionPresetCount; ++i) {
        if (g_uiState.renderWidth == kRenderResolutionPresets[i].width &&
            g_uiState.renderHeight == kRenderResolutionPresets[i].height) return i;
    }
    return -1;
}

// The shape one eye is shown at. The panel sets the scale, but the frustum sets the
// aspect: it is the frustum the application renders, and the two are not the same shape
// (Quest 3 0.935 vs 0.941, PS VR2 0.980 vs 1.049). Showing a frustum-shaped render in a
// panel-shaped frame is a stretch, and a stretch is indistinguishable from a real one.
inline void GetDisplayEyeSize(uint32_t& width, uint32_t& height) {
    std::lock_guard stateLock(g_stateMutex);
    const HeadsetSpec& spec = GetActiveHeadsetSpec();
    width = spec.panelWidth;
    const float aspect = ActiveFovAspect();
    const long h = (aspect > 0.0f) ? lroundf((float)spec.panelWidth / aspect)
                                   : (long)spec.panelHeight;
    height = (uint32_t)(std::max)(1L, h);
}

inline int GetIpdMillimeters() {
    std::lock_guard stateLock(g_stateMutex);
    return (int)(g_uiState.ipdMeters * 1000.0f + 0.5f);
}

inline void SetIpdMillimeters(int ipdMm) {
    std::lock_guard stateLock(g_stateMutex);
    ipdMm = (std::max)(0, (std::min)(200, ipdMm));
    g_uiState.ipdMeters = (float)ipdMm * 0.001f;
}

inline void AdjustIpdMillimeters(int deltaMm) {
    std::lock_guard stateLock(g_stateMutex);
    SetIpdMillimeters(GetIpdMillimeters() + deltaMm);
}

inline void SetHeadsetProfile(HeadsetProfile profile) {
    std::lock_guard stateLock(g_stateMutex);
    g_uiState.headsetProfile = profile;
    g_uiState.useAsymmetricFov = (profile != HeadsetProfile::GenericSymmetric);
    SetIpdMillimeters(GetHeadsetSpec(profile).ipdMm);
}

inline void SetSymmetricViews() {
    std::lock_guard stateLock(g_stateMutex);
    g_uiState.headsetProfile = HeadsetProfile::GenericSymmetric;
    g_uiState.useAsymmetricFov = false;
}

inline void SetAsymmetricViews() {
    std::lock_guard stateLock(g_stateMutex);
    if (g_uiState.headsetProfile == HeadsetProfile::GenericSymmetric) {
        g_uiState.headsetProfile = HeadsetProfile::Quest3;
    }
    g_uiState.useAsymmetricFov = true;
}

inline const wchar_t* GetHeadsetProfileShortName() {
    return GetActiveHeadsetSpec().shortName;
}

inline bool IsHeadsetProfileCommand(int cmd) {
    return cmd >= ID_PROFILE_FIRST && cmd < ID_PROFILE_FIRST + kHeadsetProfileCount;
}

inline bool IsFovSettingsCommand(int cmd) {
    return cmd == ID_FOV_70 || cmd == ID_FOV_90 || cmd == ID_FOV_110 ||
           cmd == ID_FOV_SYMMETRIC || cmd == ID_FOV_ASYMMETRIC ||
           IsHeadsetProfileCommand(cmd);
}

inline bool IsIpdSettingsCommand(int cmd) {
    return cmd == ID_IPD_0 || cmd == ID_IPD_58 || cmd == ID_IPD_64 ||
           cmd == ID_IPD_70 || cmd == ID_IPD_80 ||
           cmd == ID_IPD_DECREASE || cmd == ID_IPD_INCREASE;
}

// ---------------------------------------------------------------------------
// Head movement speed
// ---------------------------------------------------------------------------

struct MoveSpeedPreset { float mps;    const wchar_t* label; };
struct MoveBoostPreset { float factor; const wchar_t* label; };

inline constexpr MoveSpeedPreset kMoveSpeedPresets[] = {
    {  0.5f, L"0.&5 m/s (Inspect)" },
    {  1.0f, L"&1 m/s (Walk)" },
    {  3.0f, L"&3 m/s (Default)" },
    {  5.0f, L"&5 m/s (Brisk)" },
    { 10.0f, L"1&0 m/s (Room-scale sweep)" },
};

inline constexpr MoveBoostPreset kMoveBoostPresets[] = {
    {  2.0f, L"&2\x00D7" },
    {  4.0f, L"&4\x00D7" },
    { 10.0f, L"1&0\x00D7" },
};

inline constexpr int kMoveSpeedPresetCount =
    (int)(sizeof(kMoveSpeedPresets) / sizeof(kMoveSpeedPresets[0]));
inline constexpr int kMoveBoostPresetCount =
    (int)(sizeof(kMoveBoostPresets) / sizeof(kMoveBoostPresets[0]));

static_assert(kMoveSpeedPresetCount <= ID_MOVE_SPEED_LAST - ID_MOVE_SPEED_FIRST + 1,
              "kMoveSpeedPresets outgrew the reserved menu id block");
static_assert(kMoveBoostPresetCount <= ID_MOVE_BOOST_LAST - ID_MOVE_BOOST_FIRST + 1,
              "kMoveBoostPresets outgrew the reserved menu id block");

// Wide enough to cover a slow crawl around a controller model and a dash across
// a large play space, without letting a stuck key throw the head to infinity.
constexpr float kMinMoveSpeed = 0.05f;
constexpr float kMaxMoveSpeed = 50.0f;

inline void SetMoveSpeed(float mps) {
    std::lock_guard stateLock(g_stateMutex);
    g_uiState.moveSpeed = (std::max)(kMinMoveSpeed, (std::min)(kMaxMoveSpeed, mps));
}

// The , and . keys scale rather than add, so a notch feels the same at 0.5 m/s
// as it does at 10.
inline void ScaleMoveSpeed(float factor) {
    std::lock_guard stateLock(g_stateMutex);
    SetMoveSpeed(g_uiState.moveSpeed * factor);
}

// Meters per second for this frame. `boosted` is the Shift key.
inline float GetMoveSpeed(bool boosted) {
    std::lock_guard stateLock(g_stateMutex);
    return boosted ? g_uiState.moveSpeed * g_uiState.moveBoost : g_uiState.moveSpeed;
}

inline bool IsMoveSpeedCommand(int cmd) {
    return cmd >= ID_MOVE_SPEED_FIRST && cmd < ID_MOVE_SPEED_FIRST + kMoveSpeedPresetCount;
}

inline bool IsMoveBoostCommand(int cmd) {
    return cmd >= ID_MOVE_BOOST_FIRST && cmd < ID_MOVE_BOOST_FIRST + kMoveBoostPresetCount;
}

// ---------------------------------------------------------------------------
// Input polling
// ---------------------------------------------------------------------------

struct InputPollingPreset {
    const char*    id;          // settings key
    const wchar_t* menuLabel;
};

// Held keys come from GetAsyncKeyState, which ignores focus, so reach is entirely a matter
// of which foreground window the gate accepts.
inline constexpr InputPollingPreset kInputPollingPresets[] = {
    { "preview",    L"&Preview Window Only" },
    { "app",        L"Preview or &Application Window" },
    { "background", L"&Background (poll while unfocused)" },
};

inline constexpr int kInputPollingPresetCount =
    (int)(sizeof(kInputPollingPresets) / sizeof(kInputPollingPresets[0]));

static_assert(kInputPollingPresetCount <= ID_INPUT_POLLING_LAST - ID_INPUT_POLLING_FIRST + 1,
              "kInputPollingPresets outgrew the reserved menu id block");

inline bool IsInputPollingCommand(int cmd) {
    return cmd >= ID_INPUT_POLLING_FIRST && cmd < ID_INPUT_POLLING_FIRST + kInputPollingPresetCount;
}

// ---------------------------------------------------------------------------
// Mirror rate
// ---------------------------------------------------------------------------

// Sentinel for "no cap", kept well above any real refresh rate so the comparison in
// PreviewFrameDue never has to special-case it.
inline constexpr int kPreviewRateEveryFrame = 10000;

struct PreviewRatePreset { int fps; const wchar_t* label; };
inline constexpr PreviewRatePreset kPreviewRatePresets[] = {
    { kPreviewRateEveryFrame, L"&Every Frame (no cap)" },
    { 90, L"&90 Hz" },
    { 60, L"&60 Hz" },
    { 30, L"&30 Hz" },
    { 15, L"1&5 Hz" },
    {  0, L"&Off (freeze mirror)" },
};
inline constexpr int kPreviewRatePresetCount =
    (int)(sizeof(kPreviewRatePresets) / sizeof(kPreviewRatePresets[0]));

static_assert(kPreviewRatePresetCount <= ID_PREVIEW_RATE_LAST - ID_PREVIEW_RATE_FIRST + 1,
              "kPreviewRatePresets outgrew the reserved menu id block");

inline bool IsPreviewRateCommand(int cmd) {
    return cmd >= ID_PREVIEW_RATE_FIRST && cmd < ID_PREVIEW_RATE_FIRST + kPreviewRatePresetCount;
}

// Whether the mirror is allowed to update on this frame. Called once per frame from
// xrEndFrame; the answer has to hold for the whole frame, since the eye composite and the
// quad layers of one frame have to be recorded together or not at all.
inline bool PreviewFrameDue() {
    std::lock_guard stateLock(g_stateMutex);
    const int fps = g_uiState.previewFps;
    if (fps <= 0) return false;
    if (fps >= kPreviewRateEveryFrame) return true;

    static LARGE_INTEGER freq = []() { LARGE_INTEGER f; QueryPerformanceFrequency(&f); return f; }();
    static long long nextTick = 0;
    LARGE_INTEGER now;
    QueryPerformanceCounter(&now);
    if (now.QuadPart < nextTick) return false;
    const long long period = freq.QuadPart / fps;
    // Re-base rather than accumulate when we have fallen more than a period behind, so a
    // stall (or a rate change) cannot leave a backlog that fires on every frame after it.
    if (nextTick == 0 || now.QuadPart - nextTick > period) nextTick = now.QuadPart + period;
    else nextTick += period;
    return true;
}

// ---------------------------------------------------------------------------
// Zoom and pan
//
// Both are properties of the image, not of the window: zooming in scales the eyes
// inside whatever size the user dragged the window to and lets the window clip the
// overflow, the way an image viewer does.
// ---------------------------------------------------------------------------

// What the preview is currently showing, as the render path last measured it. Zoom and
// pan are expressed against these, so the input handlers need them as much as the blit
// does -- the runtime republishes them before acting on a wheel or a drag.
struct PreviewGeometry {
    int clientW = 0, clientH = 0;    // preview window client area
    int contentW = 0, contentH = 0;  // the eyes' layout at 1:1
};

inline PreviewGeometry g_previewGeom;

// Where the content lands in the client area, in client pixels.
struct PreviewRect { float x, y, w, h; };

// Anything below a tenth stops being a preview; anything above 8x is past the point
// where one panel pixel covers a screen tile.
constexpr float kMinZoom = 0.1f;
constexpr float kMaxZoom = 8.0f;

// ---------------------------------------------------------------------------
// Settings persistence
//
// The runtime is a DLL with no dependable shutdown hook -- the preview window
// deliberately never posts a quit message, and hosts get killed outright -- so
// every change writes the file instead of waiting for an exit that may not come.
// ---------------------------------------------------------------------------

inline const char* ViewModeName(ViewMode m) {
    switch (m) {
        case ViewMode::LeftEyeOnly:  return "left";
        case ViewMode::RightEyeOnly: return "right";
        case ViewMode::BothEyes:     break;
    }
    return "both";
}

inline ViewMode ViewModeFromName(const char* s, ViewMode def) {
    if (strcmp(s, "left") == 0)  return ViewMode::LeftEyeOnly;
    if (strcmp(s, "right") == 0) return ViewMode::RightEyeOnly;
    if (strcmp(s, "both") == 0)  return ViewMode::BothEyes;
    return def;
}

inline const char* DisplayLayoutName(DisplayLayout l) {
    switch (l) {
        case DisplayLayout::OverUnder: return "over_under";
        case DisplayLayout::Anaglyph:  return "anaglyph";
        case DisplayLayout::SideBySide: break;
    }
    return "side_by_side";
}

inline DisplayLayout DisplayLayoutFromName(const char* s, DisplayLayout def) {
    if (strcmp(s, "over_under") == 0)   return DisplayLayout::OverUnder;
    if (strcmp(s, "anaglyph") == 0)     return DisplayLayout::Anaglyph;
    if (strcmp(s, "side_by_side") == 0) return DisplayLayout::SideBySide;
    return def;
}

inline const char* HeadsetProfileName(HeadsetProfile p) {
    return GetHeadsetSpec(p).id;
}

inline HeadsetProfile HeadsetProfileFromName(const char* s, HeadsetProfile def) {
    int i = FindHeadsetSpec(s);
    return i < 0 ? def : (HeadsetProfile)i;
}

inline const char* InputPollingName(InputPolling p) {
    int i = (int)p;
    if (i < 0 || i >= kInputPollingPresetCount) i = (int)InputPolling::AppForeground;
    return kInputPollingPresets[i].id;
}

inline InputPolling InputPollingFromName(const char* s, InputPolling def) {
    for (int i = 0; i < kInputPollingPresetCount; ++i) {
        if (strcmp(s, kInputPollingPresets[i].id) == 0) return (InputPolling)i;
    }
    return def;
}

// Empty until LoadSettings() runs, which makes every SaveSettings() before that
// a no-op -- startup can't write defaults over a file it hasn't read yet.
inline std::filesystem::path g_settingsPath;
inline std::string g_lastSettingsJson;

inline std::string SerializeSettings() {
    std::lock_guard stateLock(g_stateMutex);
    char buf[1024];
    snprintf(buf, sizeof(buf),
        "{\n"
        "  \"view_mode\": \"%s\",\n"
        "  \"layout\": \"%s\",\n"
        "  \"headset_profile\": \"%s\",\n"
        "  \"render_width\": %d,\n"
        "  \"render_height\": %d,\n"
        "  \"asymmetric_fov\": %s,\n"
        "  \"fov_degrees\": %d,\n"
        "  \"ipd_mm\": %d,\n"
        "  \"zoom_mode\": \"%s\",\n"
        "  \"zoom_scale\": %.3f,\n"
        "  \"window_width\": %d,\n"
        "  \"window_height\": %d,\n"
        "  \"full_render\": %s,\n"
        "  \"show_stats\": %s,\n"
        "  \"move_speed\": %.2f,\n"
        "  \"move_boost\": %.2f,\n"
        "  \"input_polling\": \"%s\",\n"
        "  \"preview_fps\": %d\n"
        "}\n",
        ViewModeName(g_uiState.viewMode),
        DisplayLayoutName(g_uiState.displayLayout),
        HeadsetProfileName(g_uiState.headsetProfile),
        g_uiState.renderWidth,
        g_uiState.renderHeight,
        g_uiState.useAsymmetricFov ? "true" : "false",
        g_uiState.fovDegrees,
        GetIpdMillimeters(),
        g_uiState.fitToWindow ? "fit" : "scale",
        g_uiState.zoomLevel,
        g_uiState.windowWidth,
        g_uiState.windowHeight,
        g_uiState.showFullRender ? "true" : "false",
        g_uiState.showStats ? "true" : "false",
        g_uiState.moveSpeed,
        g_uiState.moveBoost,
        InputPollingName(g_uiState.inputPolling),
        g_uiState.previewFps);
    return buf;
}

inline void SaveSettings() {
    if (g_settingsPath.empty()) return;

    std::string json = SerializeSettings();
    if (json == g_lastSettingsJson) return;

    auto temporary = g_settingsPath;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
    FILE* f = nullptr;
    if (_wfopen_s(&f, temporary.c_str(), L"wb") != 0 || !f) return;
    const bool written = fwrite(json.data(), 1, json.size(), f) == json.size();
    const bool closed = fclose(f) == 0;
    if (!written || !closed ||
        !MoveFileExW(temporary.c_str(), g_settingsPath.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH)) {
        DeleteFileW(temporary.c_str());
        return;
    }
    g_lastSettingsJson = std::move(json);
}

inline void ApplySettingsJson(const char* text) {
    std::lock_guard stateLock(g_stateMutex);
    json::Object o(text);

    // Profile first: it resets FOV symmetry and IPD, so the saved values for
    // those have to land after it.
    std::string profile = o.string("headset_profile");
    if (!profile.empty()) {
        SetHeadsetProfile(HeadsetProfileFromName(profile.c_str(), g_uiState.headsetProfile));
    }

    // A missing pair is an older settings file: keep the new performance default. 0x0 is
    // the explicit "Headset Native" mode; positive custom values are accepted even when
    // they are not one of the menu presets.
    const bool hasRenderResolution = o.has("render_width") || o.has("render_height");
    if (hasRenderResolution) {
        const int width = o.number("render_width", g_uiState.renderWidth);
        const int height = o.number("render_height", g_uiState.renderHeight);
        SetRenderResolution(width, height);
    }

    g_uiState.viewMode = ViewModeFromName(
        o.string("view_mode").c_str(), g_uiState.viewMode);
    g_uiState.displayLayout = DisplayLayoutFromName(
        o.string("layout").c_str(), g_uiState.displayLayout);

    g_uiState.useAsymmetricFov = o.boolean("asymmetric_fov", g_uiState.useAsymmetricFov);
    g_uiState.fovDegrees = (std::max)(30, (std::min)(170,
        o.number("fov_degrees", g_uiState.fovDegrees)));
    SetIpdMillimeters(o.number("ipd_mm", GetIpdMillimeters()));

    // The mirror always opens fitted at 100%: zoom/scale is a transient inspection
    // tool, and a session that ended zoomed or panned must not pin the next one there.
    // "zoom_mode"/"zoom_scale" in the file are deliberately ignored on load.
    g_uiState.fitToWindow = true;
    g_uiState.zoomLevel = 1.0f;
    g_uiState.panX = 0.0f;
    g_uiState.panY = 0.0f;

    // Left unclamped: the desktop this was saved on may not be the one it reopens on,
    // so the caller fits it to the current work area instead.
    g_uiState.windowWidth = (std::max)(0, o.number("window_width", g_uiState.windowWidth));
    g_uiState.windowHeight = (std::max)(0, o.number("window_height", g_uiState.windowHeight));
    g_uiState.showFullRender = o.boolean("full_render", g_uiState.showFullRender);
    g_uiState.showStats = o.boolean("show_stats", g_uiState.showStats);

    SetMoveSpeed(o.number("move_speed", g_uiState.moveSpeed));
    g_uiState.moveBoost = (std::max)(1.0f, (std::min)(50.0f,
        o.number("move_boost", g_uiState.moveBoost)));

    g_uiState.inputPolling = InputPollingFromName(
        o.string("input_polling").c_str(), g_uiState.inputPolling);

    g_uiState.previewFps = (std::max)(0, (std::min)(kPreviewRateEveryFrame,
        (int)o.number("preview_fps", g_uiState.previewFps)));
}

// Restore saved settings from `dataDir` and arm SaveSettings().
inline void LoadSettings(const std::filesystem::path& dataDir) {
    std::error_code error;
    std::filesystem::create_directories(dataDir, error);
    const auto path = dataDir / L"settings.json";

    FILE* f = nullptr;
    if (_wfopen_s(&f, path.c_str(), L"rb") == 0 && f) {
        char buf[2048];
        size_t n = fread(buf, 1, sizeof(buf) - 1, f);
        buf[n] = '\0';
        fclose(f);
        ApplySettingsJson(buf);
    }

    g_settingsPath = std::move(path);
    g_lastSettingsJson.clear();
}

// Dark mode colors
namespace Colors {
    const COLORREF Background = RGB(30, 30, 30);
    const COLORREF Surface = RGB(45, 45, 45);
    const COLORREF Primary = RGB(100, 149, 237);  // Cornflower blue
    const COLORREF Text = RGB(230, 230, 230);
    const COLORREF TextSecondary = RGB(160, 160, 160);
    const COLORREF Border = RGB(60, 60, 60);
    const COLORREF Accent = RGB(0, 150, 136);  // Teal
}

// Enable dark title bar (Windows 10 1809+)
inline void EnableDarkTitleBar(HWND hwnd) {
    BOOL darkMode = TRUE;
    DwmSetWindowAttribute(hwnd, 20, &darkMode, sizeof(darkMode));
    DwmSetWindowAttribute(hwnd, 19, &darkMode, sizeof(darkMode));
}

// Set window border color
inline void SetWindowBorderColor(HWND hwnd, COLORREF color) {
    DwmSetWindowAttribute(hwnd, 34, &color, sizeof(color));
}

// Set caption/title bar color
inline void SetCaptionColor(HWND hwnd, COLORREF color) {
    DwmSetWindowAttribute(hwnd, 35, &color, sizeof(color));
}

// ---------------------------------------------------------------------------
// Dark menu bar
//
// DWM only darkens the title bar; the classic Win32 menu bar stays white. Two
// pieces fix that: the undocumented uxtheme ordinals switch the process's
// popup menus to dark, and the WM_UAH* custom-draw messages DefWindowProc
// sends for a themed menu bar let us paint the bar itself. Both are the
// established technique (Notepad++, WinMerge) on Windows 10 1809+; on older
// systems every call quietly degrades to the light menu.
// ---------------------------------------------------------------------------

#ifndef WM_UAHDRAWMENU
#define WM_UAHDRAWMENU     0x0091
#define WM_UAHDRAWMENUITEM 0x0092
#endif

// Layouts of the undocumented WM_UAH* payloads, transcribed from the
// win32-darkmode reference. Only the fields read below matter.
struct UAHMenu {
    HMENU hmenu;
    HDC hdc;
    DWORD dwFlags;
};

struct UAHMenuItemMetrics {
    union {
        struct { DWORD cx, cy; } rgsizeBar[2];
        struct { DWORD cx, cy; } rgsizePopup[4];
    };
};

struct UAHMenuPopupMetrics {
    DWORD rgcx[4];
    DWORD fUpdateMaxWidths : 2;
};

struct UAHMenuItem {
    int iPosition;
    UAHMenuItemMetrics umim;
    UAHMenuPopupMetrics umpm;
};

struct UAHDrawMenuItem {
    DRAWITEMSTRUCT dis;
    UAHMenu um;
    UAHMenuItem umi;
};

inline HBRUSH MenuBarBrush() {
    static HBRUSH brush = CreateSolidBrush(Colors::Surface);
    return brush;
}

inline HBRUSH MenuBarHotBrush() {
    static HBRUSH brush = CreateSolidBrush(RGB(70, 70, 70));
    return brush;
}

// Switch this process's popup menus to dark: SetPreferredAppMode(ForceDark) and
// FlushMenuThemes, exported from uxtheme by ordinal only. The runtime lives in
// the host application's process, so this would also darken a Win32 context
// menu the host opened itself - acceptable for a developer tool, and games do
// not use Win32 menus.
inline void EnableDarkPopupMenus() {
    static bool applied = false;
    if (applied) return;
    applied = true;
    HMODULE uxtheme = LoadLibraryExW(L"uxtheme.dll", nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
    if (!uxtheme) return;
    // Ordinal 135 is SetPreferredAppMode(enum) on 1903+ and AllowDarkModeForApp(BOOL)
    // on 1809; the ForceDark value 2 reads as TRUE there, so one call serves both.
    using SetPreferredAppModeFn = int(WINAPI*)(int);
    using FlushMenuThemesFn = void(WINAPI*)();
    auto setPreferredAppMode =
        (SetPreferredAppModeFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(135));
    auto flushMenuThemes =
        (FlushMenuThemesFn)GetProcAddress(uxtheme, MAKEINTRESOURCEA(136));
    if (setPreferredAppMode) setPreferredAppMode(2 /* ForceDark */);
    if (flushMenuThemes) flushMenuThemes();
}

// Custom-draw the menu bar dark. Returns true when the message was consumed and
// *result holds the answer; the window procedure handles everything else as usual.
inline bool HandleDarkMenuMessage(HWND hwnd, UINT msg, WPARAM wParam, LPARAM lParam,
                                  LRESULT* result) {
    switch (msg) {
        case WM_UAHDRAWMENU: {
            if (!GetMenu(hwnd)) return false;
            const UAHMenu* menu = (const UAHMenu*)lParam;
            MENUBARINFO mbi = { sizeof(mbi) };
            if (!menu || !menu->hdc || !GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return false;
            RECT rcWindow{};
            GetWindowRect(hwnd, &rcWindow);
            RECT rc = mbi.rcBar;                       // screen -> window coordinates
            OffsetRect(&rc, -rcWindow.left, -rcWindow.top);
            rc.top -= 1;                               // cover the theme's top seam too
            FillRect(menu->hdc, &rc, MenuBarBrush());
            *result = TRUE;
            return true;
        }
        case WM_UAHDRAWMENUITEM: {
            const UAHDrawMenuItem* item = (const UAHDrawMenuItem*)lParam;
            if (!item || !item->dis.hDC) return false;
            wchar_t label[256] = {};
            MENUITEMINFOW mii = { sizeof(mii) };
            mii.fMask = MIIM_STRING;
            mii.dwTypeData = label;
            mii.cch = (UINT)(sizeof(label) / sizeof(label[0]) - 1);
            GetMenuItemInfoW(item->um.hmenu, (UINT)item->umi.iPosition, TRUE, &mii);

            DWORD textFlags = DT_CENTER | DT_SINGLELINE | DT_VCENTER;
            if (item->dis.itemState & ODS_NOACCEL) textFlags |= DT_HIDEPREFIX;

            const bool hot = (item->dis.itemState & (ODS_HOTLIGHT | ODS_SELECTED)) != 0;
            const bool disabled = (item->dis.itemState & (ODS_GRAYED | ODS_DISABLED)) != 0;
            FillRect(item->dis.hDC, &item->dis.rcItem, hot ? MenuBarHotBrush() : MenuBarBrush());
            SetBkMode(item->dis.hDC, TRANSPARENT);
            SetTextColor(item->dis.hDC, disabled ? Colors::TextSecondary : Colors::Text);
            RECT rcText = item->dis.rcItem;
            DrawTextW(item->dis.hDC, label, -1, &rcText, textFlags);
            *result = TRUE;
            return true;
        }
        // DefWindowProc paints a light 1px line between the menu bar and the
        // client area from these two; let it run, then paint the line dark.
        case WM_NCPAINT:
        case WM_NCACTIVATE: {
            if (!GetMenu(hwnd)) return false;
            *result = DefWindowProcW(hwnd, msg, wParam, lParam);
            MENUBARINFO mbi = { sizeof(mbi) };
            if (!GetMenuBarInfo(hwnd, OBJID_MENU, 0, &mbi)) return true;
            RECT rcClient{};
            GetClientRect(hwnd, &rcClient);
            MapWindowPoints(hwnd, nullptr, (POINT*)&rcClient, 2);
            RECT rcWindow{};
            GetWindowRect(hwnd, &rcWindow);
            OffsetRect(&rcClient, -rcWindow.left, -rcWindow.top);
            RECT rcLine = rcClient;
            rcLine.bottom = rcLine.top;
            rcLine.top -= 1;
            if (HDC hdc = GetWindowDC(hwnd)) {
                FillRect(hdc, &rcLine, MenuBarBrush());
                ReleaseDC(hwnd, hdc);
            }
            return true;
        }
    }
    return false;
}

// Create the application menu
inline HMENU CreateAppMenu() {
    HMENU menuBar = CreateMenu();

    // View Menu
    HMENU viewMenu = CreatePopupMenu();
    AppendMenuW(viewMenu, MF_STRING, ID_VIEW_BOTH_EYES, L"&Both Eyes\tB");
    AppendMenuW(viewMenu, MF_STRING, ID_VIEW_LEFT_EYE, L"&Left Eye Only\tL");
    AppendMenuW(viewMenu, MF_STRING, ID_VIEW_RIGHT_EYE, L"&Right Eye Only\tR");
    AppendMenuW(viewMenu, MF_SEPARATOR, 0, nullptr);

    // Display layout submenu
    HMENU layoutMenu = CreatePopupMenu();
    AppendMenuW(layoutMenu, MF_STRING, ID_DISPLAY_SIDE_BY_SIDE, L"&Side by Side");
    AppendMenuW(layoutMenu, MF_STRING, ID_DISPLAY_OVER_UNDER, L"&Over/Under");
    AppendMenuW(layoutMenu, MF_STRING, ID_DISPLAY_ANAGLYPH, L"&Anaglyph 3D");
    AppendMenuW(viewMenu, MF_POPUP, (UINT_PTR)layoutMenu, L"Display &Layout");

    AppendMenuW(menuBar, MF_POPUP, (UINT_PTR)viewMenu, L"&View");

    // Zoom Menu
    HMENU zoomMenu = CreatePopupMenu();
    AppendMenuW(zoomMenu, MF_STRING, ID_ZOOM_FIT, L"&Fit to Window\tF");
    AppendMenuW(zoomMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(zoomMenu, MF_STRING, ID_ZOOM_25, L"25%\t1");
    AppendMenuW(zoomMenu, MF_STRING, ID_ZOOM_50, L"50%\t2");
    AppendMenuW(zoomMenu, MF_STRING, ID_ZOOM_75, L"75%\t3");
    AppendMenuW(zoomMenu, MF_STRING, ID_ZOOM_100, L"100%\t4");
    AppendMenuW(zoomMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(zoomMenu, MF_STRING, ID_ZOOM_IN, L"Zoom &In\t+");
    AppendMenuW(zoomMenu, MF_STRING, ID_ZOOM_OUT, L"Zoom &Out\t-");

    AppendMenuW(menuBar, MF_POPUP, (UINT_PTR)zoomMenu, L"&Zoom");

    // FOV Menu
    HMENU fovMenu = CreatePopupMenu();
    AppendMenuW(fovMenu, MF_STRING, ID_FOV_SYMMETRIC, L"&Symmetric Views\t8");
    AppendMenuW(fovMenu, MF_STRING, ID_FOV_ASYMMETRIC, L"&Asymmetric Views\t9");
    AppendMenuW(fovMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(fovMenu, MF_STRING, ID_FOV_70, L"70\x00B0 (Narrow)\t5");
    AppendMenuW(fovMenu, MF_STRING, ID_FOV_90, L"90\x00B0 (Normal)\t6");
    AppendMenuW(fovMenu, MF_STRING, ID_FOV_110, L"110\x00B0 (Wide)\t7");
    AppendMenuW(fovMenu, MF_SEPARATOR, 0, nullptr);

    HMENU profileMenu = CreatePopupMenu();
    for (int i = 0; i < kHeadsetProfileCount; ++i) {
        if (i == 1) AppendMenuW(profileMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(profileMenu, MF_STRING, ID_PROFILE_FIRST + i, kHeadsetSpecs[i].menuLabel);
    }
    AppendMenuW(fovMenu, MF_POPUP, (UINT_PTR)profileMenu, L"Headset &Profile");

    HMENU ipdMenu = CreatePopupMenu();
    AppendMenuW(ipdMenu, MF_STRING, ID_IPD_DECREASE, L"Decrease IPD\t[");
    AppendMenuW(ipdMenu, MF_STRING, ID_IPD_INCREASE, L"Increase IPD\t]");
    AppendMenuW(ipdMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(ipdMenu, MF_STRING, ID_IPD_0, L"0 mm (No Stereo)");
    AppendMenuW(ipdMenu, MF_STRING, ID_IPD_58, L"58 mm");
    AppendMenuW(ipdMenu, MF_STRING, ID_IPD_64, L"64 mm");
    AppendMenuW(ipdMenu, MF_STRING, ID_IPD_70, L"70 mm");
    AppendMenuW(ipdMenu, MF_STRING, ID_IPD_80, L"80 mm");
    AppendMenuW(fovMenu, MF_POPUP, (UINT_PTR)ipdMenu, L"&IPD");
    AppendMenuW(fovMenu, MF_SEPARATOR, 0, nullptr);

    AppendMenuW(fovMenu, MF_STRING, ID_VIEW_FULL_RENDER, L"Show &Full Render\tG");
    AppendMenuW(menuBar, MF_POPUP, (UINT_PTR)fovMenu, L"F&OV");

    // Tools Menu
    HMENU toolsMenu = CreatePopupMenu();
    AppendMenuW(toolsMenu, MF_STRING, ID_TOOLS_SCREENSHOT, L"Take &Screenshot\tF12");
    AppendMenuW(toolsMenu, MF_STRING, ID_TOOLS_RESET_VIEW, L"&Reset View\tHome");
    AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(toolsMenu, MF_STRING, ID_TOOLS_TOGGLE_STATS, L"Show &Statistics\tF3");
    AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);

    HMENU renderResolutionMenu = CreatePopupMenu();
    for (int i = 0; i < kRenderResolutionPresetCount; ++i) {
        if (i == 1) AppendMenuW(renderResolutionMenu, MF_SEPARATOR, 0, nullptr);
        AppendMenuW(renderResolutionMenu, MF_STRING,
                    ID_RENDER_RESOLUTION_FIRST + i, kRenderResolutionPresets[i].label);
    }
    AppendMenuW(toolsMenu, MF_POPUP, (UINT_PTR)renderResolutionMenu,
                L"Render &Resolution (per eye, restart app)");
    AppendMenuW(toolsMenu, MF_SEPARATOR, 0, nullptr);

    HMENU moveMenu = CreatePopupMenu();
    AppendMenuW(moveMenu, MF_STRING, ID_MOVE_SLOWER, L"Slo&wer\t,");
    AppendMenuW(moveMenu, MF_STRING, ID_MOVE_FASTER, L"&Faster\t.");
    AppendMenuW(moveMenu, MF_SEPARATOR, 0, nullptr);
    for (int i = 0; i < kMoveSpeedPresetCount; ++i) {
        AppendMenuW(moveMenu, MF_STRING, ID_MOVE_SPEED_FIRST + i, kMoveSpeedPresets[i].label);
    }
    AppendMenuW(moveMenu, MF_SEPARATOR, 0, nullptr);

    HMENU boostMenu = CreatePopupMenu();
    for (int i = 0; i < kMoveBoostPresetCount; ++i) {
        AppendMenuW(boostMenu, MF_STRING, ID_MOVE_BOOST_FIRST + i, kMoveBoostPresets[i].label);
    }
    AppendMenuW(moveMenu, MF_POPUP, (UINT_PTR)boostMenu, L"S&hift Multiplier");

    AppendMenuW(toolsMenu, MF_POPUP, (UINT_PTR)moveMenu, L"&Movement Speed");

    HMENU inputMenu = CreatePopupMenu();
    for (int i = 0; i < kInputPollingPresetCount; ++i) {
        AppendMenuW(inputMenu, MF_STRING,
                    ID_INPUT_POLLING_FIRST + i, kInputPollingPresets[i].menuLabel);
    }
    AppendMenuW(toolsMenu, MF_POPUP, (UINT_PTR)inputMenu, L"&Input Polling");

    HMENU rateMenu = CreatePopupMenu();
    for (int i = 0; i < kPreviewRatePresetCount; ++i) {
        AppendMenuW(rateMenu, MF_STRING, ID_PREVIEW_RATE_FIRST + i, kPreviewRatePresets[i].label);
    }
    AppendMenuW(toolsMenu, MF_POPUP, (UINT_PTR)rateMenu, L"Mirror &Rate");
    AppendMenuW(menuBar, MF_POPUP, (UINT_PTR)toolsMenu, L"&Tools");

    // Help Menu
    HMENU helpMenu = CreatePopupMenu();
    AppendMenuW(helpMenu, MF_STRING, ID_HELP_CONTROLS, L"&Controls...\tF1");
    AppendMenuW(helpMenu, MF_STRING, ID_HELP_ABOUT, L"&About OpenXR Simulator");
    AppendMenuW(menuBar, MF_POPUP, (UINT_PTR)helpMenu, L"&Help");

    return menuBar;
}

// Update menu check marks based on current state
inline void UpdateMenuState(HMENU menu) {
    const auto state = ReadState();
    // View mode checks
    CheckMenuItem(menu, ID_VIEW_BOTH_EYES,
        state.viewMode == ViewMode::BothEyes ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_VIEW_LEFT_EYE,
        state.viewMode == ViewMode::LeftEyeOnly ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_VIEW_RIGHT_EYE,
        state.viewMode == ViewMode::RightEyeOnly ? MF_CHECKED : MF_UNCHECKED);

    // Layout checks
    CheckMenuItem(menu, ID_DISPLAY_SIDE_BY_SIDE,
        state.displayLayout == DisplayLayout::SideBySide ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_DISPLAY_OVER_UNDER,
        state.displayLayout == DisplayLayout::OverUnder ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_DISPLAY_ANAGLYPH,
        state.displayLayout == DisplayLayout::Anaglyph ? MF_CHECKED : MF_UNCHECKED);

    // Zoom checks
    CheckMenuItem(menu, ID_ZOOM_FIT, state.fitToWindow ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_ZOOM_25, (!state.fitToWindow && state.zoomLevel == 0.25f) ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_ZOOM_50, (!state.fitToWindow && state.zoomLevel == 0.50f) ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_ZOOM_75, (!state.fitToWindow && state.zoomLevel == 0.75f) ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_ZOOM_100, (!state.fitToWindow && state.zoomLevel == 1.0f) ? MF_CHECKED : MF_UNCHECKED);

    // Stats toggle
    CheckMenuItem(menu, ID_TOOLS_TOGGLE_STATS,
        state.showStats ? MF_CHECKED : MF_UNCHECKED);

    // FOV checks
    CheckMenuItem(menu, ID_FOV_SYMMETRIC, !state.useAsymmetricFov ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_FOV_ASYMMETRIC, state.useAsymmetricFov ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_FOV_70, (!state.useAsymmetricFov && state.fovDegrees == 70) ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_FOV_90, (!state.useAsymmetricFov && state.fovDegrees == 90) ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_FOV_110, (!state.useAsymmetricFov && state.fovDegrees == 110) ? MF_CHECKED : MF_UNCHECKED);

    // Headset profile checks
    for (int i = 0; i < kHeadsetProfileCount; ++i) {
        CheckMenuItem(menu, ID_PROFILE_FIRST + i,
            (int)state.headsetProfile == i ? MF_CHECKED : MF_UNCHECKED);
    }

    // Input polling checks
    for (int i = 0; i < kInputPollingPresetCount; ++i) {
        CheckMenuItem(menu, ID_INPUT_POLLING_FIRST + i,
            (int)state.inputPolling == i ? MF_CHECKED : MF_UNCHECKED);
    }

    // Mirror rate checks
    for (int i = 0; i < kPreviewRatePresetCount; ++i) {
        CheckMenuItem(menu, ID_PREVIEW_RATE_FIRST + i,
            state.previewFps == kPreviewRatePresets[i].fps ? MF_CHECKED : MF_UNCHECKED);
    }

    // Render-resolution checks. A custom settings.json value intentionally leaves every
    // preset unchecked instead of pretending it is the nearest one.
    const int renderPreset = GetRenderResolutionPresetIndex();
    for (int i = 0; i < kRenderResolutionPresetCount; ++i) {
        CheckMenuItem(menu, ID_RENDER_RESOLUTION_FIRST + i,
            renderPreset == i ? MF_CHECKED : MF_UNCHECKED);
    }

    // IPD checks
    int ipdMm = GetIpdMillimeters();
    CheckMenuItem(menu, ID_IPD_0, ipdMm == 0 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_IPD_58, ipdMm == 58 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_IPD_64, ipdMm == 64 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_IPD_70, ipdMm == 70 ? MF_CHECKED : MF_UNCHECKED);
    CheckMenuItem(menu, ID_IPD_80, ipdMm == 80 ? MF_CHECKED : MF_UNCHECKED);

    // Full render toggle
    CheckMenuItem(menu, ID_VIEW_FULL_RENDER,
        state.showFullRender ? MF_CHECKED : MF_UNCHECKED);

    // Movement speed checks. Nothing is checked once , or . has walked the
    // speed off a preset, which is the honest answer.
    for (int i = 0; i < kMoveSpeedPresetCount; ++i) {
        CheckMenuItem(menu, ID_MOVE_SPEED_FIRST + i,
            fabsf(state.moveSpeed - kMoveSpeedPresets[i].mps) < 0.005f
                ? MF_CHECKED : MF_UNCHECKED);
    }
    for (int i = 0; i < kMoveBoostPresetCount; ++i) {
        CheckMenuItem(menu, ID_MOVE_BOOST_FIRST + i,
            fabsf(state.moveBoost - kMoveBoostPresets[i].factor) < 0.005f
                ? MF_CHECKED : MF_UNCHECKED);
    }
}

// Show controls help dialog
inline void ShowControlsDialog(HWND parent) {
    const wchar_t* helpText =
        L"OpenXR Simulator Controls\n"
        L"━━━━━━━━━━━━━━━━━━━━━━━━━━━━━━\n\n"
        L"Mouse Look:\n"
        L"  Click and drag to look around\n"
        L"  ESC to release mouse capture\n\n"
        L"Movement (WASD):\n"
        L"  W/S - Forward/Backward\n"
        L"  A/D - Strafe Left/Right\n"
        L"  Q/E - Up/Down\n"
        L"  Shift - Hold to move faster\n"
        L"  , / . - Slower/Faster\n"
        L"  Tools \x2192 Movement Speed - Presets and Shift multiplier\n"
        L"  Tools \x2192 Input Polling - Whether keys work while unfocused\n\n"
        L"View Controls:\n"
        L"  B - Both eyes\n"
        L"  L - Left eye only\n"
        L"  R - Right eye only\n\n"
        L"Preview Scaling (independent of render resolution):\n"
        L"  F - Fit to the window (upscale or downscale, never stretched)\n"
        L"  1-4 - Zoom presets (25%-100% of panel resolution)\n"
        L"  +/- - Zoom in/out\n"
        L"  Mouse wheel - Zoom at the cursor\n"
        L"  Middle-drag - Pan while zoomed in\n\n"
        L"FOV:\n"
        L"  5 - 70\x00B0 (Narrow)\n"
        L"  6 - 90\x00B0 (Normal)\n"
        L"  7 - 110\x00B0 (Wide)\n"
        L"  8 - Symmetric views\n"
        L"  9 - Asymmetric views\n"
        L"  [ / ] - Decrease/Increase IPD\n"
        L"  Headset Profile menu - Asymmetric FOV presets\n"
        L"  IPD menu - Eye separation presets\n"
        L"  G - Toggle full render\n\n"
        L"Other:\n"
        L"  F12 - Screenshot\n"
        L"  F3 - Toggle stats\n"
        L"  Home - Reset view (head pose, zoom and pan)";

    MessageBoxW(parent, helpText, L"Controls", MB_OK | MB_ICONINFORMATION);
}

// Show about dialog
inline void ShowAboutDialog(HWND parent) {
    const wchar_t* aboutText =
        L"OpenXR Simulator\n"
        L"Version 1.0\n\n"
        L"A desktop-based OpenXR runtime for testing\n"
        L"and development without VR hardware.\n\n"
        L"Features:\n"
        L"  D3D11 and D3D12 support\n"
        L"  Mouse + WASD controls\n"
        L"  Stereo preview with zoom\n"
        L"  MCP integration for diagnostics";

    MessageBoxW(parent, aboutText, L"About OpenXR Simulator", MB_OK | MB_ICONINFORMATION);
}

// The client size to open the preview window at, from a SINGLE EYE source laid out for
// the current view mode. Only ever the starting shape: zoom scales the image inside the
// window, so nothing resizes the window afterwards except a layout change.
inline void CalculateWindowSize(int srcWidth, int srcHeight, int& outWidth, int& outHeight) {
    const auto state = ReadState();
    switch (state.viewMode) {
        case ViewMode::BothEyes:
            if (state.displayLayout == DisplayLayout::SideBySide) {
                // Two eyes side by side: double the width
                outWidth = srcWidth * 2;
                outHeight = srcHeight;
            } else if (state.displayLayout == DisplayLayout::OverUnder) {
                // Two eyes stacked: double the height
                outWidth = srcWidth;
                outHeight = srcHeight * 2;
            } else { // Anaglyph - both eyes overlap in same frame
                outWidth = srcWidth;
                outHeight = srcHeight;
            }
            break;
        case ViewMode::LeftEyeOnly:
        case ViewMode::RightEyeOnly:
            // Single eye: just use the single eye dimensions
            outWidth = srcWidth;
            outHeight = srcHeight;
            break;
    }

    // Ensure minimum size
    outWidth = (std::max)(outWidth, 320);
    outHeight = (std::max)(outHeight, 240);
}

// Scale at which the whole image just fits the window, and so also the scale fit mode
// runs at -- which is what makes +/- start from what the window was already showing.
//
// Each of these takes the geometry to measure rather than reading g_previewGeom, because
// the render path lays a frame out off geometry of its own and reaches them through
// ComputePreviewRect. g_previewGeom belongs to the input handlers and is not necessarily
// populated on the thread compositing the frame.
inline float FitScale(const PreviewGeometry& g) {
    if (g.contentW <= 0 || g.contentH <= 0 || g.clientW <= 0 || g.clientH <= 0) return 1.0f;
    return (std::min)((float)g.clientW / (float)g.contentW,
                      (float)g.clientH / (float)g.contentH);
}
inline float FitScale() { return FitScale(g_previewGeom); }

// D3D viewport coordinates are bounded to +-32768, and on a headset whose panel pair is
// already 5120 wide the image's own size crosses that well before kMaxZoom does. Cap the
// scale rather than hand the rasterizer a rect it cannot address.
inline float MaxZoom(const PreviewGeometry& g) {
    const int span = (std::max)(g.contentW, g.contentH);
    if (span <= 0) return kMaxZoom;
    return (std::min)(kMaxZoom, 32000.0f / (float)span);
}
inline float MaxZoom() { return MaxZoom(g_previewGeom); }

// The scale in force, whichever mode is on.
inline float EffectiveScale(const PreviewGeometry& g) {
    std::lock_guard stateLock(g_stateMutex);
    return g_uiState.fitToWindow ? FitScale(g)
                                 : (std::min)(g_uiState.zoomLevel, MaxZoom(g));
}
inline float EffectiveScale() {
    std::lock_guard stateLock(g_stateMutex);
    return EffectiveScale(g_previewGeom);
}

// Keep the image covering the window: there is nothing to pan while it is smaller than
// the client area, and once it is larger its edges may not come inside it.
inline void ClampPan() {
    std::lock_guard stateLock(g_stateMutex);
    const PreviewGeometry& g = g_previewGeom;
    const float scale = EffectiveScale();
    const float maxX = (std::max)(0.0f, ((float)g.contentW * scale - (float)g.clientW) * 0.5f);
    const float maxY = (std::max)(0.0f, ((float)g.contentH * scale - (float)g.clientH) * 0.5f);
    g_uiState.panX = (std::max)(-maxX, (std::min)(maxX, g_uiState.panX));
    g_uiState.panY = (std::max)(-maxY, (std::min)(maxY, g_uiState.panY));
}

// Read-only, so the render path can lay out a frame off its own geometry rather than
// through g_previewGeom, which the input handlers own.
inline PreviewRect ComputePreviewRect(const PreviewGeometry& g) {
    std::lock_guard stateLock(g_stateMutex);
    if (g.contentW <= 0 || g.contentH <= 0 || g.clientW <= 0 || g.clientH <= 0) {
        return { 0.0f, 0.0f, (float)(std::max)(g.clientW, 1), (float)(std::max)(g.clientH, 1) };
    }
    // Fit mode needs no case of its own: EffectiveScale hands back the scale at which the
    // image just fits, so the centring below leaves the bars.
    const float scale = EffectiveScale(g);
    PreviewRect r;
    r.w = (float)g.contentW * scale;
    r.h = (float)g.contentH * scale;
    const float maxX = (std::max)(0.0f, (r.w - (float)g.clientW) * 0.5f);
    const float maxY = (std::max)(0.0f, (r.h - (float)g.clientH) * 0.5f);
    r.x = ((float)g.clientW - r.w) * 0.5f + (std::max)(-maxX, (std::min)(maxX, g_uiState.panX));
    r.y = ((float)g.clientH - r.h) * 0.5f + (std::max)(-maxY, (std::min)(maxY, g_uiState.panY));
    return r;
}

inline PreviewRect ComputePreviewRect() {
    std::lock_guard stateLock(g_stateMutex);
    const PreviewGeometry& g = g_previewGeom;
    if (g.contentW > 0 && g.contentH > 0 && g.clientW > 0 && g.clientH > 0) {
        if (g_uiState.fitToWindow) g_uiState.panX = g_uiState.panY = 0.0f;
        else                       ClampPan();
    }
    return ComputePreviewRect(g);
}

// Move to an absolute scale, keeping whatever sits under (anchorX, anchorY) in the client
// area where it is. Leaving "fit to window" therefore starts from the scale the window was
// already showing, so the first step is one notch rather than a jump.
inline void ZoomAbout(float scale, float anchorX, float anchorY) {
    std::lock_guard stateLock(g_stateMutex);
    const PreviewRect before = ComputePreviewRect();
    const float u = before.w > 0.0f ? (anchorX - before.x) / before.w : 0.5f;
    const float v = before.h > 0.0f ? (anchorY - before.y) / before.h : 0.5f;

    g_uiState.fitToWindow = false;
    g_uiState.zoomLevel = (std::max)(kMinZoom, (std::min)(MaxZoom(), scale));

    const PreviewGeometry& g = g_previewGeom;
    const float w = (float)g.contentW * g_uiState.zoomLevel;
    const float h = (float)g.contentH * g_uiState.zoomLevel;
    g_uiState.panX = (anchorX - u * w) - ((float)g.clientW - w) * 0.5f;
    g_uiState.panY = (anchorY - v * h) - ((float)g.clientH - h) * 0.5f;
    ClampPan();
}

// Zoom about the middle of the window, for the menu items and the keyboard.
inline void SetZoom(float scale) {
    std::lock_guard stateLock(g_stateMutex);
    ZoomAbout(scale, (float)g_previewGeom.clientW * 0.5f, (float)g_previewGeom.clientH * 0.5f);
}

// Steps scale rather than add, so a notch feels the same at 25% as it does at 400%.
inline void ZoomBy(float factor) {
    std::lock_guard stateLock(g_stateMutex);
    SetZoom(EffectiveScale() * factor);
}

inline void SetFitToWindow() {
    std::lock_guard stateLock(g_stateMutex);
    g_uiState.fitToWindow = true;
    g_uiState.panX = g_uiState.panY = 0.0f;
}

// Handle menu commands - returns true if handled
inline bool HandleMenuCommand(HWND hwnd, WPARAM wParam,
    std::function<void()> resizeCallback = nullptr,
    std::function<void()> screenshotCallback = nullptr,
    std::function<void()> resetViewCallback = nullptr,
    std::function<void(int)> settingsChangedCallback = nullptr) {

    std::unique_lock stateLock(g_stateMutex);
    int cmd = LOWORD(wParam);
    bool needsResize = false;
    bool settingsChanged = false;
    bool invalidate = false;

    switch (cmd) {
        // View modes
        case ID_VIEW_BOTH_EYES:
            g_uiState.viewMode = ViewMode::BothEyes;
            needsResize = true;
            break;

        case ID_VIEW_LEFT_EYE:
            g_uiState.viewMode = ViewMode::LeftEyeOnly;
            needsResize = true;
            break;

        case ID_VIEW_RIGHT_EYE:
            g_uiState.viewMode = ViewMode::RightEyeOnly;
            needsResize = true;
            break;

        // Display layouts
        case ID_DISPLAY_SIDE_BY_SIDE:
            g_uiState.displayLayout = DisplayLayout::SideBySide;
            needsResize = true;
            break;

        case ID_DISPLAY_OVER_UNDER:
            g_uiState.displayLayout = DisplayLayout::OverUnder;
            needsResize = true;
            break;

        case ID_DISPLAY_ANAGLYPH:
            g_uiState.displayLayout = DisplayLayout::Anaglyph;
            needsResize = true;
            break;

        // Zoom presets. These scale the image, so the window is left alone.
        case ID_ZOOM_FIT:
            SetFitToWindow();
            break;

        case ID_ZOOM_25:
            SetZoom(0.25f);
            break;

        case ID_ZOOM_50:
            SetZoom(0.50f);
            break;

        case ID_ZOOM_75:
            SetZoom(0.75f);
            break;

        case ID_ZOOM_100:
            SetZoom(1.0f);
            break;

        case ID_ZOOM_IN:
            ZoomBy(1.25f);
            break;

        case ID_ZOOM_OUT:
            ZoomBy(1.0f / 1.25f);
            break;

        // FOV options
        case ID_FOV_70:
            g_uiState.headsetProfile = HeadsetProfile::GenericSymmetric;
            g_uiState.useAsymmetricFov = false;
            g_uiState.fovDegrees = 70;
            settingsChanged = true;
            break;

        case ID_FOV_90:
            g_uiState.headsetProfile = HeadsetProfile::GenericSymmetric;
            g_uiState.useAsymmetricFov = false;
            g_uiState.fovDegrees = 90;
            settingsChanged = true;
            break;

        case ID_FOV_110:
            g_uiState.headsetProfile = HeadsetProfile::GenericSymmetric;
            g_uiState.useAsymmetricFov = false;
            g_uiState.fovDegrees = 110;
            settingsChanged = true;
            break;

        case ID_FOV_SYMMETRIC:
            SetSymmetricViews();
            settingsChanged = true;
            break;

        case ID_FOV_ASYMMETRIC:
            SetAsymmetricViews();
            settingsChanged = true;
            break;

        case ID_IPD_0:
            SetIpdMillimeters(0);
            settingsChanged = true;
            break;

        case ID_IPD_58:
            SetIpdMillimeters(58);
            settingsChanged = true;
            break;

        case ID_IPD_64:
            SetIpdMillimeters(64);
            settingsChanged = true;
            break;

        case ID_IPD_70:
            SetIpdMillimeters(70);
            settingsChanged = true;
            break;

        case ID_IPD_80:
            SetIpdMillimeters(80);
            settingsChanged = true;
            break;

        case ID_IPD_DECREASE:
            AdjustIpdMillimeters(-1);
            settingsChanged = true;
            break;

        case ID_IPD_INCREASE:
            AdjustIpdMillimeters(1);
            settingsChanged = true;
            break;

        // Render options
        case ID_VIEW_FULL_RENDER:
            g_uiState.showFullRender = !g_uiState.showFullRender;
            needsResize = true;
            break;

        // Tools
        case ID_TOOLS_SCREENSHOT:
            stateLock.unlock();
            if (screenshotCallback) screenshotCallback();
            return true;

        case ID_TOOLS_RESET_VIEW:
            stateLock.unlock();
            if (resetViewCallback) resetViewCallback();
            return true;

        case ID_TOOLS_TOGGLE_STATS: {
            g_uiState.showStats = !g_uiState.showStats;
            stateLock.unlock();
            SaveSettings();
            HMENU menuT = GetMenu(hwnd);
            if (menuT) UpdateMenuState(menuT);
            return true;
        }

        // Help
        case ID_HELP_CONTROLS:
            stateLock.unlock();
            ShowControlsDialog(hwnd);
            return true;

        case ID_HELP_ABOUT:
            stateLock.unlock();
            ShowAboutDialog(hwnd);
            return true;

        // Movement speed
        case ID_MOVE_SLOWER:
            ScaleMoveSpeed(1.0f / 1.25f);
            break;

        case ID_MOVE_FASTER:
            ScaleMoveSpeed(1.25f);
            break;

        default:
            if (IsMoveSpeedCommand(cmd)) {
                SetMoveSpeed(kMoveSpeedPresets[cmd - ID_MOVE_SPEED_FIRST].mps);
                break;
            }
            if (IsMoveBoostCommand(cmd)) {
                g_uiState.moveBoost = kMoveBoostPresets[cmd - ID_MOVE_BOOST_FIRST].factor;
                break;
            }
            if (IsInputPollingCommand(cmd)) {
                g_uiState.inputPolling = (InputPolling)(cmd - ID_INPUT_POLLING_FIRST);
                break;
            }
            if (IsPreviewRateCommand(cmd)) {
                g_uiState.previewFps = kPreviewRatePresets[cmd - ID_PREVIEW_RATE_FIRST].fps;
                // The client area just changed hands between the frame path and WM_PAINT.
                invalidate = true;
                break;
            }
            if (IsRenderResolutionCommand(cmd)) {
                const RenderResolutionPreset& preset =
                    kRenderResolutionPresets[cmd - ID_RENDER_RESOLUTION_FIRST];
                SetRenderResolution(preset.width, preset.height);
                // Resolution changes must affect workload, never the amount of desktop the
                // preview occupies. Force the independent fill mode when selecting one.
                SetFitToWindow();
                settingsChanged = true;
                break;
            }
            if (!IsHeadsetProfileCommand(cmd)) return false;
            SetHeadsetProfile((HeadsetProfile)(cmd - ID_PROFILE_FIRST));
            settingsChanged = true;
            break;
    }

    stateLock.unlock();
    if (invalidate) InvalidateRect(hwnd, nullptr, TRUE);
    if (needsResize && resizeCallback) {
        resizeCallback();
    }

    if (settingsChanged && settingsChangedCallback) {
        settingsChangedCallback(cmd);
    }

    SaveSettings();

    // Update menu checkmarks
    HMENU menu = GetMenu(hwnd);
    if (menu) UpdateMenuState(menu);

    return true;
}

// Handle keyboard shortcuts - returns true if handled
inline bool HandleKeyboardShortcut(HWND hwnd, WPARAM vk,
    std::function<void()> resizeCallback = nullptr,
    std::function<void()> screenshotCallback = nullptr,
    std::function<void()> resetViewCallback = nullptr,
    std::function<void(int)> settingsChangedCallback = nullptr) {

    switch (vk) {
        case 'B':
            return HandleMenuCommand(hwnd, ID_VIEW_BOTH_EYES, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case 'L':
            return HandleMenuCommand(hwnd, ID_VIEW_LEFT_EYE, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case 'R':
            return HandleMenuCommand(hwnd, ID_VIEW_RIGHT_EYE, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case 'F':
            return HandleMenuCommand(hwnd, ID_ZOOM_FIT, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case '1':
            return HandleMenuCommand(hwnd, ID_ZOOM_25, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case '2':
            return HandleMenuCommand(hwnd, ID_ZOOM_50, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case '3':
            return HandleMenuCommand(hwnd, ID_ZOOM_75, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case '4':
            return HandleMenuCommand(hwnd, ID_ZOOM_100, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case '5':
            return HandleMenuCommand(hwnd, ID_FOV_70, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case '6':
            return HandleMenuCommand(hwnd, ID_FOV_90, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case '7':
            return HandleMenuCommand(hwnd, ID_FOV_110, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case '8':
            return HandleMenuCommand(hwnd, ID_FOV_SYMMETRIC, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case '9':
            return HandleMenuCommand(hwnd, ID_FOV_ASYMMETRIC, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case 'G':
            return HandleMenuCommand(hwnd, ID_VIEW_FULL_RENDER, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case VK_OEM_PLUS:
        case VK_ADD:
            return HandleMenuCommand(hwnd, ID_ZOOM_IN, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case VK_OEM_MINUS:
        case VK_SUBTRACT:
            return HandleMenuCommand(hwnd, ID_ZOOM_OUT, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case VK_OEM_4:
            return HandleMenuCommand(hwnd, ID_IPD_DECREASE, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case VK_OEM_6:
            return HandleMenuCommand(hwnd, ID_IPD_INCREASE, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case VK_OEM_COMMA:
            return HandleMenuCommand(hwnd, ID_MOVE_SLOWER, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case VK_OEM_PERIOD:
            return HandleMenuCommand(hwnd, ID_MOVE_FASTER, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case VK_F1:
            ShowControlsDialog(hwnd);
            return true;
        case VK_F3:
            return HandleMenuCommand(hwnd, ID_TOOLS_TOGGLE_STATS, resizeCallback, screenshotCallback, resetViewCallback, settingsChangedCallback);
        case VK_F12:
            if (screenshotCallback) screenshotCallback();
            return true;
        case VK_HOME:
            if (resetViewCallback) resetViewCallback();
            return true;
    }
    return false;
}

// Handle mouse wheel for zoom. Anchored on the cursor, so scrolling over a detail walks
// into it instead of into the middle of the image. `anchor` is in client pixels; the
// caller is responsible for having published the current geometry.
inline bool HandleMouseWheel(HWND hwnd, short delta, float anchorX, float anchorY) {
    ZoomAbout(EffectiveScale() * (delta > 0 ? 1.1f : 1.0f / 1.1f), anchorX, anchorY);
    SaveSettings();

    HMENU menu = GetMenu(hwnd);
    if (menu) UpdateMenuState(menu);

    return true;
}

// Apply dark theme to window
inline void ApplyDarkTheme(HWND hwnd) {
    EnableDarkTitleBar(hwnd);
    EnableDarkPopupMenus();
    SetCaptionColor(hwnd, Colors::Surface);
    SetWindowBorderColor(hwnd, Colors::Border);

    HMENU menu = CreateAppMenu();
    SetMenu(hwnd, menu);
    UpdateMenuState(menu);

    RedrawWindow(hwnd, nullptr, nullptr, RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
}

// Extra information for the title bar when "Show Statistics" is on.
struct StatsInfo {
    int  sourceW = 0, sourceH = 0;   // per-eye XR swapchain dims
    int  clientW = 0, clientH = 0;   // preview window client dims
    float headX = 0, headY = 0, headZ = 0;
    float yawDeg = 0, pitchDeg = 0, rollDeg = 0;
    // Measured once per xrEndFrame that contains a valid stereo projection.
    // This is deliberately independent of desktop repaint/readback cadence.
    bool projectionTimingActive = false;
    double projectionFps = 0.0;
    double latestFrameMs = 0.0;
    double p50FrameMs = 0.0;
    double p95FrameMs = 0.0;
    uint32_t projectionTimingSamples = 0;
};

// Briefly-shown "Screenshot saved" notice. Set by the capture path; the
// title-bar updater displays it for a few seconds, then it expires.
inline std::wstring g_lastScreenshotPath;
inline std::atomic<DWORD> g_lastScreenshotTickMs{0};
constexpr DWORD     kScreenshotNoticeMs = 4000;

inline void NotifyScreenshotSaved(const std::wstring& path) {
    std::lock_guard lock(g_stateMutex);
    g_lastScreenshotPath = path;
    g_lastScreenshotTickMs = GetTickCount();
}

// Update window title with current state info. When `stats` is non-null and
// "Show Statistics" is on, the title gains a stats suffix. A recent screenshot
// notice (within kScreenshotNoticeMs) is prepended.
inline void UpdateWindowTitle(HWND hwnd, const StatsInfo* stats = nullptr) {
    const auto state = ReadState();
    wchar_t title[768];

    const wchar_t* viewModeStr = L"Both Eyes";
    if (state.viewMode == ViewMode::LeftEyeOnly) viewModeStr = L"Left Eye";
    else if (state.viewMode == ViewMode::RightEyeOnly) viewModeStr = L"Right Eye";

    // Fit is resolution-independent, so there is no percentage to show for it.
    wchar_t zoomStr[32];
    if (state.fitToWindow) {
        swprintf_s(zoomStr, L"Fit");
    } else {
        swprintf_s(zoomStr, L"%d%%", (int)(state.zoomLevel * 100.0f + 0.5f));
    }

    uint32_t renderW = 0, renderH = 0;
    GetRenderResolution(renderW, renderH);

    wchar_t base[512];
    if (stats && stats->projectionTimingActive && stats->projectionTimingSamples > 0) {
        swprintf_s(base,
            L"OpenXR Simulator - XR %.1f FPS avg | %.1f ms now - %s - %s - %s - %ux%u/eye - %dmm",
            stats->projectionFps, stats->latestFrameMs,
            viewModeStr, zoomStr, GetHeadsetProfileShortName(), renderW, renderH,
            GetIpdMillimeters());
    } else {
        swprintf_s(base, L"OpenXR Simulator - XR FPS waiting for stereo projection - %s - %s - %s - %ux%u/eye - %dmm",
                   viewModeStr, zoomStr, GetHeadsetProfileShortName(), renderW, renderH,
                   GetIpdMillimeters());
    }

    wchar_t statsSuffix[256] = L"";
    if (state.showStats && stats) {
        swprintf_s(statsSuffix,
            L"  |  XR p50 %.1f ms p95 %.1f ms (%u)  Src %dx%d  Win %dx%d  Spd %.2g m/s  Head (%.2f,%.2f,%.2f) Yaw %.0f° Pitch %.0f°",
            stats->p50FrameMs, stats->p95FrameMs, stats->projectionTimingSamples,
            stats->sourceW, stats->sourceH, stats->clientW, stats->clientH,
            state.moveSpeed,
            stats->headX, stats->headY, stats->headZ,
            stats->yawDeg, stats->pitchDeg);
    }

    // Optional one-shot "Screenshot saved" notice
    std::wstring notice;
    {
        std::lock_guard lock(g_stateMutex);
        if (g_lastScreenshotTickMs != 0 && GetTickCount() - g_lastScreenshotTickMs < kScreenshotNoticeMs)
            notice = g_lastScreenshotPath;
        else {
            g_lastScreenshotPath.clear();
            g_lastScreenshotTickMs = 0;
        }
    }
    const bool showNotice = !notice.empty();
    if (showNotice) {
        swprintf_s(title, L"[Screenshot saved → %s]  %s%s",
                   notice.c_str(), base, statsSuffix);
    } else {
        swprintf_s(title, L"%s%s", base, statsSuffix);
    }

    SetWindowTextW(hwnd, title);
}

} // namespace ui
