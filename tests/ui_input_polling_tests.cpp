#include "Pch.h"
#include "control/legacy/WindowsRuntimeSettings.h"
namespace settings = simxr::legacy::settings;


static int s_failures = 0;

static void Check(bool condition, const char* message)
{
	if (condition)
		return;
	std::fprintf(stderr, "FAIL: %s\n", message);
	++s_failures;
}

static void ResetSettings()
{
	settings::ApplySettingsJson(
		R"({"headset_profile":"quest3","render_width":1280,"render_height":1400,"asymmetric_fov":true,
            "fov_degrees":90,"ipd_mm":64,"move_speed":3,"move_boost":4,"input_polling":"app"})");
}

int main()
{
	Check(settings::ReadState().inputPolling == settings::InputPolling::AppForeground,
		  "input polling defaults to accepting the application's own window");

	// A reordered table would silently rename every saved setting.
	Check(settings::kInputPollingPresetCount == 3, "three input polling modes");
	Check(std::strcmp(settings::InputPollingName(settings::InputPolling::PreviewFocused), "preview") == 0 &&
			  std::strcmp(settings::InputPollingName(settings::InputPolling::AppForeground), "app") == 0 &&
			  std::strcmp(settings::InputPollingName(settings::InputPolling::Background), "background") == 0,
		  "each enum value names its own table row");

	for (int i = 0; i < settings::kInputPollingPresetCount; ++i)
	{
		const settings::InputPolling mode = static_cast<settings::InputPolling>(i);
		Check(settings::InputPollingFromName(settings::InputPollingName(mode),
											 settings::InputPolling::PreviewFocused) == mode,
			  "every mode survives a name round trip");
	}

	Check(settings::InputPollingFromName("nonsense", settings::InputPolling::Background) ==
			  settings::InputPolling::Background,
		  "an unreadable name leaves the current mode alone");

	ResetSettings();
	settings::ApplySettingsJson("{\"input_polling\": \"background\"}");
	Check(settings::ReadState().inputPolling == settings::InputPolling::Background,
		  "the mode is restored from settings");

	ResetSettings();
	settings::ApplySettingsJson("{\"view_mode\": \"both\"}");
	Check(settings::ReadState().inputPolling == settings::InputPolling::AppForeground,
		  "an older settings file keeps the default mode");

	if (s_failures != 0)
		return 1;
	std::puts("ui_input_polling_tests: PASS");
	return 0;
}
