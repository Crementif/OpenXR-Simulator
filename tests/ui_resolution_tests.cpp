#include "ui_enhancements.h"

#include <cmath>
#include <cstdio>
#include <thread>
#include <future>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

bool Near(float a, float b) {
    return std::fabs(a - b) < 0.001f;
}

// The property the whole snap exists for: what we recommend has the shape of what we
// report. One pixel of rounding either way is all the slack there is.
bool MatchesFovAspect(uint32_t width, uint32_t height) {
    if (width == 0 || height == 0) return false;
    const float want = (float)width / ui::ActiveFovAspect();
    return std::fabs((float)height - want) <= 1.0f;
}

} // namespace

int main() {
    ui::g_uiState = ui::UIState{};

    uint32_t width = 0, height = 0;
    ui::GetRenderResolution(width, height);
    Check(width == 1280 && MatchesFovAspect(width, height),
          "performance preset keeps its width and takes its height from the FOV");

    ui::SetHeadsetProfile(ui::HeadsetProfile::Quest3);
    ui::SetRenderResolution(0, 0);
    ui::GetRenderResolution(width, height);
    Check(width == 2064 && MatchesFovAspect(width, height),
          "native mode follows the active headset panel width");

    ui::SetHeadsetProfile(ui::HeadsetProfile::ValveIndex);
    ui::GetRenderResolution(width, height);
    Check(width == 1440 && MatchesFovAspect(width, height),
          "native mode updates when the headset profile changes");

    // A profile switch moves the frustum, so it has to move the recommendation with it --
    // this is the case that used to advertise Quest 3 pixels in an Index shape.
    ui::SetRenderResolution(1280, 1400);
    ui::SetHeadsetProfile(ui::HeadsetProfile::Quest3);
    ui::GetRenderResolution(width, height);
    const uint32_t quest3Height = height;
    Check(width == 1280 && MatchesFovAspect(width, height),
          "an explicit resolution keeps its width across profiles");
    ui::SetHeadsetProfile(ui::HeadsetProfile::ValveIndex);
    ui::GetRenderResolution(width, height);
    Check(width == 1280 && MatchesFovAspect(width, height) && height != quest3Height,
          "an explicit resolution still re-shapes for the new profile");

    // Symmetric geometry cuts a square out of the plane, so a square is what to recommend.
    ui::SetHeadsetProfile(ui::HeadsetProfile::GenericSymmetric);
    ui::GetRenderResolution(width, height);
    Check(width == 1280 && height == 1280,
          "the symmetric profile recommends a square target");

    ui::SetHeadsetProfile(ui::HeadsetProfile::Quest3);
    ui::SetRenderResolution(1, 9000);
    ui::GetRenderResolution(width, height);
    Check(width == (uint32_t)ui::kMinRenderWidth && MatchesFovAspect(width, height),
          "custom render resolution is clamped to runtime limits");

    // The Index frustum is tall enough that a full-width target overflows maxImageRect.
    ui::SetHeadsetProfile(ui::HeadsetProfile::ValveIndex);
    ui::SetRenderResolution(ui::kMaxRenderExtent, ui::kMaxRenderExtent);
    ui::GetRenderResolution(width, height);
    Check(height == (uint32_t)ui::kMaxRenderExtent && width < (uint32_t)ui::kMaxRenderExtent &&
          MatchesFovAspect(width, height),
          "a derived height past maxImageRect gives up width instead of shape");

    // Fit mode: the image is scaled uniformly and centred, never stretched to the client
    // aspect. 4128x2208 is wider than 16:9, so it meets the sides and leaves bars above
    // and below.
    ui::g_previewGeom = { 1920, 1080, 4128, 2208 };
    ui::SetFitToWindow();
    const ui::PreviewRect wide = ui::ComputePreviewRect();
    const float wideScale = 1920.0f / 4128.0f;
    Check(Near(wide.x, 0.0f) && Near(wide.w, 1920.0f) &&
          Near(wide.h, 2208.0f * wideScale) &&
          Near(wide.y, (1080.0f - 2208.0f * wideScale) * 0.5f),
          "fit mode letterboxes content wider than the client area");

    // ... and the other way round: a tall image meets the top and bottom with bars at
    // the sides.
    ui::g_previewGeom = { 1920, 1080, 1000, 1200 };
    const ui::PreviewRect tall = ui::ComputePreviewRect();
    const float tallScale = 1080.0f / 1200.0f;
    Check(Near(tall.y, 0.0f) && Near(tall.h, 1080.0f) &&
          Near(tall.w, 1000.0f * tallScale) &&
          Near(tall.x, (1920.0f - 1000.0f * tallScale) * 0.5f),
          "fit mode pillarboxes content taller than the client area");

    ui::g_previewGeom = { 640, 360, 1920, 1080 };
    const ui::PreviewRect downscaled = ui::ComputePreviewRect();
    Check(Near(downscaled.w, 640.0f) && Near(downscaled.h, 360.0f),
          "fit mode downscales a larger render to the client area");

    ui::g_previewGeom = { 3840, 2160, 960, 540 };
    const ui::PreviewRect upscaled = ui::ComputePreviewRect();
    Check(Near(upscaled.w, 3840.0f) && Near(upscaled.h, 2160.0f),
          "fit mode upscales a smaller render to the client area");

    ui::g_uiState = ui::UIState{};
    ui::ApplySettingsJson(
        "{\"render_width\": 1500, \"render_height\": 1600, \"zoom_mode\": \"fit\"}");
    ui::GetRenderResolution(width, height);
    Check(width == 1500 && MatchesFovAspect(width, height) && ui::g_uiState.fitToWindow,
          "custom settings restore resolution and fit mode");

    ui::g_uiState = ui::UIState{};
    ui::ApplySettingsJson("{\"zoom_mode\": \"scale\", \"zoom_scale\": 1.0}");
    ui::GetRenderResolution(width, height);
    Check(width == 1280 && MatchesFovAspect(width, height) && ui::g_uiState.fitToWindow,
          "older settings migrate to the performance resolution and fit mode");

    const auto settingsDirectory = std::filesystem::temp_directory_path() /
        (L"simxr-settings-" + std::to_wstring(GetCurrentProcessId()));
    std::filesystem::create_directory(settingsDirectory);
    ui::LoadSettings(settingsDirectory);
    ui::SaveSettings();
    const auto saved = ui::g_lastSettingsJson;
    auto temporary = ui::g_settingsPath;
    temporary += L".tmp." + std::to_wstring(GetCurrentProcessId());
    std::filesystem::create_directory(temporary);
    ui::g_uiState.showStats = !ui::g_uiState.showStats;
    ui::SaveSettings();
    Check(!saved.empty() && ui::g_lastSettingsJson == saved,
          "failed settings save preserves the successful-save cache");
    std::filesystem::remove(temporary);
    ui::SaveSettings();
    Check(ui::g_lastSettingsJson == ui::SerializeSettings() && ui::g_lastSettingsJson != saved,
          "failed settings save can retry and replace the previous destination");
    const auto expectedStats = ui::g_uiState.showStats;
    ui::g_uiState.showStats = !expectedStats;
    ui::LoadSettings(settingsDirectory);
    Check(ui::g_uiState.showStats == expectedStats, "replacement settings can be reloaded");
    ui::g_settingsPath.clear();
    std::filesystem::remove_all(settingsDirectory);

    ui::SetHeadsetProfile(ui::HeadsetProfile::Quest3);
    std::thread writer([] {
        for (int i = 0; i < 10000; ++i)
            ui::SetHeadsetProfile(i % 2 ? ui::HeadsetProfile::Quest3 : ui::HeadsetProfile::GenericSymmetric);
    });
    bool consistent = true;
    for (int i = 0; i < 10000; ++i) {
        const auto state = ui::ReadState();
        consistent &= state.useAsymmetricFov == (state.headsetProfile != ui::HeadsetProfile::GenericSymmetric);
        consistent &= Near(state.ipdMeters, ui::GetHeadsetSpec(state.headsetProfile).ipdMm * .001f);
    }
    writer.join();
    Check(consistent, "preview settings snapshots preserve whole profile updates");
    std::promise<void> observed;
    auto ready = observed.get_future();
    std::thread reader;
    ui::HandleMenuCommand(nullptr, ui::ID_TOOLS_SCREENSHOT, {}, [&] {
        reader = std::thread([&] { (void)ui::ReadState(); observed.set_value(); });
        Check(ready.wait_for(std::chrono::seconds(1)) == std::future_status::ready,
              "window callbacks run without the preview settings lock");
    });
    reader.join();

    if (failures != 0) return 1;
    std::puts("ui_resolution_tests: PASS");
    return 0;
}
