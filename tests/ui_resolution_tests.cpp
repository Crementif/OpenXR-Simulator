#include "control/legacy/WindowsRuntimeSettings.h"
namespace settings = simxr::legacy::settings;

#include <cmath>
#include <cstdio>
#include <thread>

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
    const float want = (float)width / settings::ActiveFovAspect();
    return std::fabs((float)height - want) <= 1.0f;
}

} // namespace

int main() {
    settings::g_settings = settings::RuntimeSettings{};

    uint32_t width = 0, height = 0;
    settings::GetRenderResolution(width, height);
    Check(width == 1280 && MatchesFovAspect(width, height),
          "performance preset keeps its width and takes its height from the FOV");

    settings::SetHeadsetProfile(settings::HeadsetProfile::Quest3);
    settings::SetRenderResolution(0, 0);
    settings::GetRenderResolution(width, height);
    Check(width == 2064 && MatchesFovAspect(width, height),
          "native mode follows the active headset panel width");

    settings::SetHeadsetProfile(settings::HeadsetProfile::ValveIndex);
    settings::GetRenderResolution(width, height);
    Check(width == 1440 && MatchesFovAspect(width, height),
          "native mode updates when the headset profile changes");

    // A profile switch moves the frustum, so it has to move the recommendation with it --
    // this is the case that used to advertise Quest 3 pixels in an Index shape.
    settings::SetRenderResolution(1280, 1400);
    settings::SetHeadsetProfile(settings::HeadsetProfile::Quest3);
    settings::GetRenderResolution(width, height);
    const uint32_t quest3Height = height;
    Check(width == 1280 && MatchesFovAspect(width, height),
          "an explicit resolution keeps its width across profiles");
    settings::SetHeadsetProfile(settings::HeadsetProfile::ValveIndex);
    settings::GetRenderResolution(width, height);
    Check(width == 1280 && MatchesFovAspect(width, height) && height != quest3Height,
          "an explicit resolution still re-shapes for the new profile");

    // Symmetric geometry cuts a square out of the plane, so a square is what to recommend.
    settings::SetHeadsetProfile(settings::HeadsetProfile::GenericSymmetric);
    settings::GetRenderResolution(width, height);
    Check(width == 1280 && height == 1280,
          "the symmetric profile recommends a square target");

    settings::SetHeadsetProfile(settings::HeadsetProfile::Quest3);
    settings::SetRenderResolution(1, 9000);
    settings::GetRenderResolution(width, height);
    Check(width == (uint32_t)settings::kMinRenderWidth && MatchesFovAspect(width, height),
          "custom render resolution is clamped to runtime limits");

    // The Index frustum is tall enough that a full-width target overflows maxImageRect.
    settings::SetHeadsetProfile(settings::HeadsetProfile::ValveIndex);
    settings::SetRenderResolution(settings::kMaxRenderExtent, settings::kMaxRenderExtent);
    settings::GetRenderResolution(width, height);
    Check(height == (uint32_t)settings::kMaxRenderExtent && width < (uint32_t)settings::kMaxRenderExtent &&
          MatchesFovAspect(width, height),
          "a derived height past maxImageRect gives up width instead of shape");

    settings::g_settings = settings::RuntimeSettings{};
    settings::ApplySettingsJson(
        "{\"render_width\": 1500, \"render_height\": 1600, \"zoom_mode\": \"fit\"}");
    settings::GetRenderResolution(width, height);
    Check(width == 1500 && MatchesFovAspect(width, height),
          "custom settings restore resolution");

    settings::g_settings = settings::RuntimeSettings{};
    settings::ApplySettingsJson("{\"zoom_mode\": \"scale\", \"zoom_scale\": 1.0}");
    settings::GetRenderResolution(width, height);
    Check(width == 1280 && MatchesFovAspect(width, height),
          "older settings migrate to the performance resolution");

    settings::SetHeadsetProfile(settings::HeadsetProfile::Quest3);
    std::thread writer([] {
        for (int i = 0; i < 10000; ++i)
            settings::SetHeadsetProfile(i % 2 ? settings::HeadsetProfile::Quest3 : settings::HeadsetProfile::GenericSymmetric);
    });
    bool consistent = true;
    for (int i = 0; i < 10000; ++i) {
        const auto state = settings::ReadState();
        consistent &= state.useAsymmetricFov == (state.headsetProfile != settings::HeadsetProfile::GenericSymmetric);
        consistent &= Near(state.ipdMeters, settings::GetHeadsetSpec(state.headsetProfile).ipdMm * .001f);
    }
    writer.join();
    Check(consistent, "preview settings snapshots preserve whole profile updates");

    if (failures != 0) return 1;
    std::puts("ui_resolution_tests: PASS");
    return 0;
}
