#include "control/legacy/WindowsRuntimeSettings.h"
namespace settings = simxr::legacy::settings;

#include <cstdio>
#include <cstring>
#include <string>

namespace {

int failures = 0;

void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

} // namespace

int main() {
    settings::g_settings = settings::RuntimeSettings{};

    Check(settings::g_settings.inputPolling == settings::InputPolling::AppForeground,
          "input polling defaults to accepting the application's own window");

    // A reordered table would silently rename every saved setting.
    Check(settings::kInputPollingPresetCount == 3, "three input polling modes");
    Check(std::strcmp(settings::InputPollingName(settings::InputPolling::PreviewFocused), "preview") == 0 &&
          std::strcmp(settings::InputPollingName(settings::InputPolling::AppForeground), "app") == 0 &&
          std::strcmp(settings::InputPollingName(settings::InputPolling::Background), "background") == 0,
          "each enum value names its own table row");

    for (int i = 0; i < settings::kInputPollingPresetCount; ++i) {
        const settings::InputPolling mode = (settings::InputPolling)i;
        Check(settings::InputPollingFromName(settings::InputPollingName(mode),
                                       settings::InputPolling::PreviewFocused) == mode,
              "every mode survives a name round trip");
    }

    Check(settings::InputPollingFromName("nonsense", settings::InputPolling::Background) ==
              settings::InputPolling::Background,
          "an unreadable name leaves the current mode alone");

    settings::g_settings = settings::RuntimeSettings{};
    settings::ApplySettingsJson("{\"input_polling\": \"background\"}");
    Check(settings::g_settings.inputPolling == settings::InputPolling::Background,
          "the mode is restored from settings");

    settings::g_settings = settings::RuntimeSettings{};
    settings::ApplySettingsJson("{\"view_mode\": \"both\"}");
    Check(settings::g_settings.inputPolling == settings::InputPolling::AppForeground,
          "an older settings file keeps the default mode");

    if (failures != 0) return 1;
    std::puts("ui_input_polling_tests: PASS");
    return 0;
}
