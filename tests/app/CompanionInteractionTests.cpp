#include "app/UiPch.h"
#include "interaction/Cases.h"
#ifdef _WIN32
#include "app/platform/windows/LaunchSupport.h"
#include "app/platform/windows/UpdateTransport.h"
#include <fcntl.h>
#include <io.h>
#endif
#include <iostream>

using namespace simxr::tests;

int main(int argc, char** argv)
{
	try
	{
#ifdef _WIN32
		if (argc == 2 && std::string_view(argv[1]).starts_with("--update-worker="))
		{
			const auto response = simxr::app::windows::FetchLatestReleaseDirect(std::string_view(argv[1]).substr(16));
			_setmode(_fileno(stdout), _O_BINARY);
			std::cout.write(response.data(), static_cast<std::streamsize>(response.size()));
			return std::cout ? 0 : 1;
		}
#else
		(void)argc;
#endif
#ifdef __linux__
		if (std::getenv("SIMXR_TAB_LAUNCH_FIXTURE"))
			return RunTabLaunchFixture();
		if (std::getenv("SIMXR_TAB_LAUNCH_QA"))
		{
			CheckTabLaunches();
			return 0;
		}
#endif
		using RunCase = void (*)(const char*, const char*);
		const RunCase preferences = [](auto, auto) {
			Interaction test;
			CheckPreferences(test);
		};

		struct Case
		{
			const char* environment;
			RunCase run;
		};

		const Case cases[]{
			{"SIMXR_DISTRIBUTION_QA",
			 [](auto, auto executable) {
				 CheckDistributionOnboarding();
				 CheckProfileCopyButton(executable);
				 CheckProfileClipboard(executable);
			 }},
			{"SIMXR_HEADSET_LAUNCH_QA",
			 [](auto, auto) {
				 CheckHeadsetLaunchProfile();
				 CheckHeadsetRestartProfile();
			 }},
			{"SIMXR_RIG_POINTER_QA", [](auto, auto) { CheckRigPointer(); }},
			{"SIMXR_TABS_LIVE_FIRST",
			 [](auto first, auto) {
				 const auto* second = std::getenv("SIMXR_TABS_LIVE_SECOND");
				 Require(*first && second && *second, "Two live tab endpoints are required");
				 std::vector<std::string> endpoints{first, second};
				 if (const auto* third = std::getenv("SIMXR_TABS_LIVE_THIRD"))
				 {
					 const auto* fourth = std::getenv("SIMXR_TABS_LIVE_FOURTH");
					 Require(*third && fourth && *fourth, "Both additional live tab endpoints are required");
					 endpoints.emplace_back(third);
					 endpoints.emplace_back(fourth);
				 }
				 CheckPortableLiveTabs(endpoints, QaDirectory("SIMXR_TABS_LIVE_DIRECTORY"));
			 }},
#ifdef __linux__
			{"SIMXR_TABS_QA", [](auto, auto) { CheckApplicationTabs(); }},
			{"SIMXR_TABS_GPU_FIRST",
			 [](auto first, auto) {
				 const auto* second = std::getenv("SIMXR_TABS_GPU_SECOND");
				 Require(second && *second, "Two GPU tab endpoints are required");
				 if (std::getenv("SIMXR_TABS_INPUT_QA"))
					 CheckGpuTabInput(first, second);
				 else
					 CheckMultiGpuTabs(first, second);
			 }},
			{"SIMXR_UI_PILOT_ENDPOINT", [](auto endpoint, auto) { PilotLiveRuntime(endpoint); }},
			{"SIMXR_HAPTICS_QA", [](auto, auto) { CheckLiveHaptics(); }},
			{"SIMXR_DISCOVERY_FAILURE_QA", [](auto, auto) { CheckDiscoveryFailure(); }},
#endif
			{"SIMXR_SHELL_QA", [](auto, auto) { CheckShellScenario(); }},
			{"SIMXR_RECORDING_ARCHIVE_QA", [](auto, auto) { CheckArchiveRecordingUi(); }},
			{"SIMXR_HEADSET_PAGE_QA", [](auto, auto) { CheckPhysicalHeadsetPage(); }},
			{"SIMXR_TRACKER_COLLECTION_QA", [](auto, auto) { CheckExpandedTrackerUi(); }},
			{"SIMXR_DEVICE_HEADER_QA", [](auto, auto) { ConfigureObservedDevices(); }},
			{"SIMXR_TIMELINE_ENDPOINT",
			 [](auto endpoint, auto) {
				 Interaction test;
				 test.live = true;
				 test.runtime.ConnectEndpoint(endpoint);
				 Eventually(
					 [&] {
						 test.Frame();
						 return test.runtime.Snapshot().selectedReady;
					 },
					 "Timeline app preview absent");
				 CheckTimelineWorkflow(test);
			 }},
			{"SIMXR_UPDATE_STARTUP_QA", [](auto, auto) { CheckUpdateStartup(); }},
			{"SIMXR_BUDGET_QA", preferences},
			{"SIMXR_MCP_UI_QA", preferences},
			{"SIMXR_UPDATE_UI_QA", preferences},
			{"SIMXR_PREVIEW_PILOT_ENDPOINT", [](auto endpoint, auto) { CheckConnectedPreview(endpoint); }},
			{"SIMXR_DEBUG_QA_ENDPOINT", [](auto endpoint, auto) { CheckDebuggerWorkflow(endpoint); }}};
		for (const auto& test : cases)
			if (const auto* value = std::getenv(test.environment))
			{
				test.run(value, argv[0]);
				return 0;
			}
#ifdef __linux__
		if (const auto executable = std::getenv("SIMXR_LAUNCH_QA_EXECUTABLE");
			executable && std::getenv("SIMXR_PICKER_QA"))
		{
			CheckAppPicker(executable);
			return 0;
		}
#endif
#ifdef _WIN32
		const auto launchExecutable = windows::Environment("SIMXR_LAUNCH_QA_EXECUTABLE");
		const auto executable = launchExecutable.c_str();
#else
		const auto executable = std::getenv("SIMXR_LAUNCH_QA_EXECUTABLE");
#endif
		if (executable && *executable)
			CheckToolbarLaunch(executable);
		else
			CheckDefaultInteraction();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
