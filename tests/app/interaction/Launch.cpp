#include "app/UiPch.h"
#include "Cases.h"
#include "runtime/Runtime.h"
#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "app/dialogs/LaunchProfilesDialog.h"
#include "app/dialogs/PreferencesDialog.h"
#include "app/services/PlayerRig.h"
#include "app/services/RecordingController.h"
#include "app/services/RuntimeSession.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/RecordingPanel.h"
#include "app/widgets/Theme.h"
#include "app/services/RunningApps.h"
#include "simxr/Version.h"
#include <iostream>
#include <random>
#ifdef _WIN32
#include "app/platform/windows/LaunchSupport.h"
#endif

namespace simxr::tests
{

void CheckToolbarLaunch(const char* executable)
{
	const auto directory = QaDirectory("SIMXR_LAUNCH_QA_DIRECTORY");
	Require(directory.is_absolute() && std::filesystem::create_directory(directory),
			"Launch QA directory must be new and absolute");
	const auto settingsPath = directory / "companion.settings";
#ifdef _WIN32
	Require(SetEnvironmentVariableW(L"SIMXR_COMPANION_SETTINGS", settingsPath.c_str()), "Set isolated settings path");
#else
	Require(::setenv("SIMXR_COMPANION_SETTINGS", settingsPath.c_str(), 1) == 0, "Set isolated settings path");
#endif
	CompanionSettings settings;
	LaunchProfile profile;
	profile.name = "Isolated launch QA";
	profile.executable = executable;
#ifdef _WIN32
	profile.runtimeManifest = windows::Environment("SIMXR_LAUNCH_QA_MANIFEST");
	profile.privateProtonPrefix = windows::Environment("SIMXR_PRIVATE_PROTON_TEST_PREFIX");
	profile.workingDirectory = windows::Environment("SIMXR_LAUNCH_QA_CWD");
	const auto argumentsText = windows::Environment("SIMXR_LAUNCH_QA_ARGUMENTS");
	const auto arguments = argumentsText.c_str();
#else
	profile.runtimeManifest = std::getenv("XR_RUNTIME_JSON") ? std::getenv("XR_RUNTIME_JSON") : "";
	if (const auto cwd = std::getenv("SIMXR_LAUNCH_QA_CWD"))
		profile.workingDirectory = cwd;
	const auto arguments = std::getenv("SIMXR_LAUNCH_QA_ARGUMENTS");
#endif
	if (std::getenv("SIMXR_LAUNCH_QA_BUNDLED"))
	{
		profile.runtimeManifest.clear();
		profile.useBundledRuntime = true;
	}
	if (arguments)
	{
		std::istringstream lines(arguments);
		for (std::string argument; std::getline(lines, argument);)
			profile.arguments.push_back(argument);
	}
	profile.environment = {{"SIMXR_DATA_DIR", PathText(directory)}};
#ifdef __linux__
	if (std::getenv("SIMXR_SOAK_QA") || std::getenv("SIMXR_RESTART_QA"))
	{
		Require(std::filesystem::path(executable).filename() == "xr_probe_linux", "Soak requires the isolated probe");
		profile.environment.emplace_back("SIMXR_PROBE_INTERACTIVE", "1");
		profile.environment.emplace_back("SIMXR_PROBE_GPU_INSPECTION", "1");
		profile.environment.emplace_back("SIMXR_PROBE_FRAMES", "120000");
		profile.environment.emplace_back("SIMXR_VULKAN_VALIDATION", "1");
	}
#endif
	settings.profiles.push_back(profile);
	settings.activeProfile = profile.name;
	SaveSettings(settingsPath, settings);
	Interaction test(true);
	for (unsigned frame = 0; frame < 4; ++frame)
		test.Frame();
	if (!profile.privateProtonPrefix.empty())
	{
		LaunchProfilesDialog editor;
		editor.Edit(settings, 0);
		bool open = true, changed = false;
		test.extraDraw = [&] {
			editor.Draw(settings, {}, open, changed);
		};
		for (unsigned frame = 0; frame < 3; ++frame)
			test.Frame();
		auto* form = FindPanel("Profile form");
		Require(form, "Private Proton launch form missing");
		ImGui::ActivateItemByID(ImHashStr("Runtime and script", 0, ImHashStr("1", 0, form->ID)));
		for (unsigned frame = 0; frame < 3; ++frame)
			test.Frame();
		ImGui::SetScrollY(form, form->ScrollMax.y);
		for (unsigned frame = 0; frame < 3; ++frame)
			test.Frame();
		Require(editor.Profile() == profile && !form->ScrollbarX,
				"Private Proton launch form lost configuration or overflows horizontally");
		test.renderer.RequestScreenshot(directory / "private-prefix-profile.bmp");
		test.Frame();
		ImGui::ClosePopupToLevel(0, true);
		test.extraDraw = {};
		test.Frame();
		if (std::getenv("SIMXR_LAUNCH_FORM_ONLY"))
		{
			test.renderer.WaitIdle();
			std::cout << "PASS private-prefix launch profile layout; no application launched\n";
			return;
		}
	}
	test.renderer.RequestScreenshot(directory / "before-launch.bmp");
	test.Frame();
	const auto* toolbar = FindPanel("##Workspace toolbar");
	Require(toolbar != nullptr, "Launch toolbar absent");
	test.Click({toolbar->Pos.x + 20, toolbar->Pos.y + ImGui::GetFrameHeight() / 2});
	const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
	Require(!popups.empty() && popups.back().Window, "App picker did not open");
	const auto profilesId = popups.back().Window->GetID("Profiles");
	const int profileIndex = 0;
	const auto profileId = ImHashData(&profileIndex, sizeof(profileIndex), profilesId);
	ImGui::ActivateItemByID(ImHashStr("##Launch profile", 0, profileId));
	test.Frame();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!test.runtime.Snapshot().selectedReady)
	{
		test.Frame();
		Require(std::chrono::steady_clock::now() < deadline, "Toolbar launch did not attach and display live pixels");
	}
	const auto state = test.runtime.Snapshot();
	Require(state.connected && state.sessionAvailable && state.selectedLayers == 2,
			"Launched stereo session not selected");
#ifdef _WIN32
	Require(state.endpoint.starts_with(R"(\\.\pipe\SimXR.Control.)"), "Launch selected a nonlocal endpoint");
#else
	Require(std::filesystem::path(state.endpoint).parent_path() == directory, "Launch selected unrelated endpoint");
#endif
	std::ofstream(directory / "attached.endpoint") << state.endpoint;
#ifdef __linux__
	if (std::getenv("SIMXR_RESTART_QA"))
	{
		CheckHeadsetRestartWorkflow(test, directory);
		test.renderer.WaitIdle();
		Require(test.validation->errors.load() == 0, "Restart workflow produced Vulkan validation errors");
		return;
	}
	if (std::getenv("SIMXR_SOAK_QA"))
		CheckSoak(test, directory);
	if (std::getenv("SIMXR_TIMELINE_QA"))
		CheckTimelineWorkflow(test);
	if (std::getenv("SIMXR_PAD_QA"))
		CheckInputPad(test);
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"); qa && std::getenv("SIMXR_POLISH_QA"))
		CapturePolish(test, qa);
	if (std::getenv("SIMXR_TIMING_QA"))
		CheckMeasuredTiming(test);
	if (std::getenv("SIMXR_HISTORY_QA"))
		CheckPreviewHistory(test);
	if (std::getenv("SIMXR_TAKE_QA"))
		CheckInputTake(test, directory);
	if (std::getenv("SIMXR_LIBRARY_QA"))
		CheckLibrary(test, directory);
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"); qa && std::getenv("SIMXR_LAYOUT_QA"))
		CheckLayouts(test, qa);
	if (std::getenv("SIMXR_SHELL_QA"))
		CheckShell(test, std::getenv("SIMXR_UI_QA_DIR"));
	if (std::getenv("SIMXR_ID_QA") || std::getenv("SIMXR_ID_FRAME_QA"))
		CheckIdConflicts(test, std::getenv("SIMXR_ID_FRAME_QA") != nullptr);
	if (std::getenv("SIMXR_INPUT_QA"))
		CheckManualEdges(state);
	if (std::getenv("SIMXR_RIG_QA"))
		CheckNativePlayerRig(state);
	if (std::getenv("SIMXR_PILOT_QA"))
		CheckPreviewPilot(test);
	if (std::getenv("SIMXR_SPATIAL_QA"))
		CheckSpatialEditing(test);
	if (std::getenv("SIMXR_BODY_QA"))
		CheckBodyRig(test);
	if (std::getenv("SIMXR_RETENTION_QA"))
		CheckSnapshotRetention(test);
#endif
	Eventually(
		[&] {
			test.Frame();
			return test.runtime.History().Size() > 0;
		},
		"Launched runtime did not populate timeline history");
	Require(test.runtime.Snapshot().error.empty(), "Launched runtime reported a control error");
	test.renderer.RequestScreenshot(directory / "toolbar-launch.bmp");
	test.Frame();
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors in launched preview");
	std::cout << "PASS toolbar launched and attached stereo runtime " << state.endpoint << '\n';
}

void CaptureLayouts(const std::filesystem::path& directory)
{
	std::filesystem::create_directories(directory);
	Interaction test;
	test.live = true;
	for (const auto size : {ImVec2{1280, 720}, ImVec2{1920, 1080}})
		for (const float scale : {1.0f, 1.5f})
		{
			test.window.Resize(static_cast<int>(size.x), static_cast<int>(size.y));
			test.workspace.SetScale(scale);
			for (int page = 0; page < 8; ++page)
			{
				test.workspace.ShowLibrary(page == 7);
				test.workspace.Select(DeviceSel{static_cast<DevicePage>(page % 7)});
				for (int frame = 0; frame < 4; ++frame)
					test.Frame();
				const bool overlay = size.x / scale < 800;
				for (const char* name :
					 {"Top bar", overlay ? "Device rail" : "Device tabs", "Inspector", "Timeline", "GPU preview"})
				{
					const auto* window = FindPanel(name);
					Require(window && !window->ScrollbarX, "Shell region has horizontal scrolling");
					Require(window->Pos.x >= 0 && window->Pos.x + window->Size.x <= size.x + 1,
							"Shell region extends beyond window");
				}
				const auto path =
					directory / (std::to_string(static_cast<int>(size.x)) + "-" +
								 std::to_string(static_cast<int>(scale * 100)) + "-" + std::to_string(page) + ".bmp");
				test.renderer.RequestScreenshot(path);
				test.Frame();
			}
		}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors during layout capture");
}

void CheckDistributionOnboarding()
{
	IgnoreRealPointer();
	Interaction test(true);
	const auto resize = [&](int width, int height) {
		const auto rebuilds = test.renderer.RebuildCount();
		test.window.Resize(width, height);
		Eventually(
			[&] {
				test.Frame();
				return test.renderer.RebuildCount() > rebuilds;
			},
			"Distribution UI resize did not rebuild the swapchain");
	};
	resize(1280, 720);
	for (unsigned frame = 0; frame < 4; ++frame)
		test.Frame();
	const auto directory = QaDirectory("SIMXR_UI_QA_DIR");
	if (!directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "distribution-empty.bmp");
		test.Frame();
	}
	auto* preview = FindPanel("Image area");
	Require(preview, "Disconnected preview is missing");
	ImGui::ActivateItemByID(preview->GetID("Add application"));
	for (unsigned frame = 0; frame < 3; ++frame)
		test.Frame();
	auto* profile = FindPanel("Launch profiles");
	Require(profile && profile->Active, "Add application did not open the launch editor");
	auto* form = FindPanel("Profile form");
	Require(form && !form->ScrollbarX, "Launch editor overflows horizontally");
	if (!directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "distribution-profile.bmp");
		test.Frame();
	}
	test.workspace.SetScale(1.5f);
	for (unsigned frame = 0; frame < 4; ++frame)
		test.Frame();
	Require(!form->ScrollbarX, "Enlarged launch editor overflows horizontally");
	if (!directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "distribution-profile-large.bmp");
		test.Frame();
	}
	resize(1920, 1080);
	Require(!form->ScrollbarX, "Large-screen launch editor overflows horizontally");
	if (!directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "distribution-profile-1080.bmp");
		test.Frame();
	}
	test.workspace.SetScale(1);
	resize(760, 600);
	Require(!form->ScrollbarX, "Narrow launch editor overflows horizontally");
	if (!directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "distribution-profile-narrow.bmp");
		test.Frame();
	}
#ifdef __linux__
	resize(1280, 720);
	for (unsigned frame = 0; frame < 3; ++frame)
		test.Frame();
	ImGui::ActivateItemByID(ImHashStr("Runtime and script", 0, ImHashStr("1", 0, form->ID)));
	for (unsigned frame = 0; frame < 3; ++frame)
		test.Frame();
	ImGui::ActivateItemByID(ImHashStr("Steam launch options", 0, ImHashStr("1", 0, form->ID)));
	for (unsigned frame = 0; frame < 3; ++frame)
		test.Frame();
	const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
	Require(popups.size() >= 2 && popups.back().Window, "Steam launch options did not open");
	if (!directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "distribution-steam.bmp");
		test.Frame();
	}
#endif
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Distribution onboarding produced Vulkan validation errors");
	std::cout << "PASS distribution onboarding\n";
}

void CheckUpdateStartup()
{
	Require(std::getenv("SIMXR_COMPANION_SETTINGS"), "Update startup check requires isolated settings");
	Require(ValidReleaseRepository(simxr::ReleaseRepository), "Update startup check needs a configured repository");
	CompanionSettings settings;
	settings.autoUpdateCheck = true;
	SaveSettings(CompanionSettingsPath(), settings);
	IgnoreRealPointer();
	Interaction test(true);
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(12);
	while (true)
	{
		test.Frame();
		const auto* notice = FindPanel("##Notices");
		if (notice && notice->Active)
			break;
		Require(std::chrono::steady_clock::now() < deadline, "Automatic update notice did not appear");
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (const auto directory = std::getenv("SIMXR_DIALOG_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(directory) / "update-startup-notice.bmp");
		test.Frame();
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Update startup produced Vulkan validation errors");
	std::cout << "PASS automatic update startup notice\n";
}

void CheckProfileCopyButton(const std::filesystem::path& executable)
{
	IgnoreRealPointer();
	Interaction test;
	CompanionSettings settings;
	LaunchProfile profile;
	profile.name = "Saved profile";
	profile.executable = PathText(std::filesystem::absolute(executable));
	settings.profiles.push_back(profile);
	LaunchProfilesDialog editor;
	editor.Edit(settings, 0);
	bool open = true, changed = false;
	test.extraDraw = [&] {
		editor.Draw(settings, {}, open, changed);
	};
	for (unsigned frame = 0; frame < 3; ++frame)
		test.Frame();
	auto* form = FindPanel("Profile form");
	Require(form, "Profile copy form missing");
	ImGui::ActivateItemByID(ImHashStr("Copy launch command", 0, ImHashStr("1", 0, form->ID)));
	test.Frame();
	Require(editor.TakeCopyCommandRequest(), "Saved profile copy command was not requested");
	Require(!editor.TakeCopyCommandRequest(), "Profile copy request was not consumed");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Profile copy produced Vulkan validation errors");
}

void CheckProfileClipboard(const std::filesystem::path& executable)
{
	Require(std::getenv("SIMXR_COMPANION_SETTINGS"), "Profile clipboard check requires isolated settings");
	CompanionSettings settings;
	LaunchProfile profile;
	profile.name = "Saved ' profile";
	profile.executable = PathText(std::filesystem::absolute(executable));
	settings.profiles.push_back(profile);
	settings.activeProfile = profile.name;
	SaveSettings(CompanionSettingsPath(), settings);
	IgnoreRealPointer();
	Interaction test(true);
	for (unsigned frame = 0; frame < 3; ++frame)
		test.Frame();
	auto* preview = FindPanel("Image area");
	Require(preview, "Profile clipboard preview missing");
	ImGui::ActivateItemByID(preview->GetID("Add application"));
	for (unsigned frame = 0; frame < 3; ++frame)
		test.Frame();
	auto* names = FindPanel("Profile names");
	Require(names, "Profile list missing");
	const int index = 0;
	ImGui::ActivateItemByID(ImHashStr("##Profile", 0, ImHashData(&index, sizeof(index), names->ID)));
	for (unsigned frame = 0; frame < 3; ++frame)
		test.Frame();
	auto* form = FindPanel("Profile form");
	Require(form, "Profile clipboard form missing");
	ImGui::ActivateItemByID(ImHashStr("Copy launch command", 0, ImHashStr("3", 0, form->ID)));
	test.Frame();
	const auto* base = SDL_GetBasePath();
	Require(base, "Cannot locate simulator for clipboard check");
#ifdef _WIN32
	const auto expected =
		ProfileLaunchCommand(Utf8Path(base) / "simxr_companion.exe", profile.name, LaunchCommandShell::PowerShell);
#else
	const auto expected =
		ProfileLaunchCommand(Utf8Path(base) / "simxr_companion", profile.name, LaunchCommandShell::Posix);
#endif
	char* clipboard = SDL_GetClipboardText();
	const bool copied = clipboard && expected == clipboard;
	SDL_free(clipboard);
	Require(copied, "Profile command did not reach the clipboard");
	Require(LoadSettings(CompanionSettingsPath()).profiles.front() == profile, "Copied profile was not saved");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Profile clipboard produced Vulkan validation errors");
}

void CheckHeadsetLaunchProfile()
{
	Interaction test(true);
	CompanionSettings settings;
	LaunchProfile profile;
	profile.name = "Headset launch";
	profile.executable = "game.exe";
	profile.arguments = {"", "line one\nline two"};
	profile.environment = {{"SIMXR_WIDTH", "64"}, {"KEEP", "unchanged"}};
	settings.profiles.push_back(profile);
	LaunchProfilesDialog editor;
	editor.Edit(settings, 0);
	bool open = true, changed = false, launched = false;
	std::optional<std::array<HeadsetEyeSize, 2>> eyes = std::array<HeadsetEyeSize, 2>{{{2048, 2160}, {2200, 2300}}};
	test.extraDraw = [&] {
		launched |= editor.Draw(settings, {}, open, changed, eyes);
	};
	for (unsigned frame = 0; frame < 3; ++frame)
		test.Frame();
	Require(settings.profiles.front() == profile && !changed,
			"Headset discovery changed launch settings without a click");
	auto* form = FindPanel("Profile form");
	Require(form, "Headset launch profile form missing");
	const auto activate = [&] {
		ImGui::ActivateItemByID(ImHashStr("Use headset size", 0, ImHashStr("1", 0, form->ID)));
		for (unsigned frame = 0; frame < 3; ++frame)
			test.Frame();
	};
	activate();
	Require(!form->ScrollbarX, "Headset launch settings overflow horizontally");
	if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "headset-launch-profile.bmp");
		test.Frame();
	}
	const auto saved = settings.profiles.front();
	const auto environment = std::map<std::string, std::string>(saved.environment.begin(), saved.environment.end());
	Require(changed && !launched && saved.arguments == profile.arguments && saved.environment.size() == 3 &&
				environment.at("SIMXR_WIDTH") == "2200" && environment.at("SIMXR_HEIGHT") == "2300" &&
				environment.at("KEEP") == "unchanged",
			"Headset sizes were not saved exactly or changed unrelated launch fields");
	changed = false;
	eyes = std::array<HeadsetEyeSize, 2>{{{9000, 2160}, {2200, 2300}}};
	test.Frame();
	activate();
	Require(!changed && settings.profiles.front() == saved, "Unsupported headset recommendation was silently clamped");
	eyes.reset();
	test.Frame();
	activate();
	Require(!changed && settings.profiles.front() == saved && !launched,
			"Disconnected headset changed saved launch dimensions");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Headset launch editor produced Vulkan validation errors");
}

void CheckHeadsetRestartProfile()
{
	Interaction test(true);
	CompanionSettings settings;
	LaunchProfile profile;
	profile.name = "Replacement game";
#ifdef _WIN32
	profile.executable = PathText(Utf8Path(SDL_GetBasePath()) / "companion_interaction_tests.exe");
#else
	profile.executable = PathText(Utf8Path(SDL_GetBasePath()) / "companion_interaction_tests");
#endif
	Require(PathProblem(profile.executable, PathKind::Executable).empty(),
			("Restart test executable is invalid: " + profile.executable).c_str());
	profile.environment = {{"KEEP", "unchanged"}};
	settings.profiles.push_back(profile);
	LaunchProfilesDialog editor;
	editor.Edit(settings, 0);
	LaunchSnapshot process{LaunchState::Running, "Original game", {}, 42};
	bool open = true, changed = false, canRestart = false;
	std::optional<std::array<HeadsetEyeSize, 2>> eyes = std::array<HeadsetEyeSize, 2>{{{2048, 2160}, {2200, 2300}}};
	test.extraDraw = [&] {
		ImGui::PushID("restart-editor");
		Require(!editor.Draw(settings, process, open, changed, eyes, canRestart),
				"Restart accidentally requested an ordinary launch");
		ImGui::PopID();
	};
	for (unsigned frame = 0; frame < 3; ++frame)
		test.Frame();
	auto* form = FindPanel("Profile form");
	Require(form, "Restart profile form missing");
	ImGui::SetScrollY(form, form->ScrollMax.y);
	test.Frame();
	test.Frame();
	const auto activate = [&](const char* label) {
		ImGui::ActivateItemByID(ImHashStr(label, 0, ImHashStr("1", 0, form->ID)));
		test.Frame();
		test.Frame();
	};
	activate("Restart with headset settings");
	Require(!editor.TakeRestartRequest(), "Unowned app allowed restart");
	canRestart = true;
	test.Frame();
	activate("Restart with headset settings");
	auto* review = FindPanel("Restart application?");
	Require(review && review->Active, "Restart review did not open");
	Require(!editor.TakeRestartRequest(), "Restart executed before confirmation");
	if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "headset-restart-review.bmp");
		test.Frame();
	}
	test.workspace.SetScale(1.25f);
	test.Frame();
	test.Frame();
	test.Frame();
	Require(!review->ScrollbarX && review->Pos.x >= 0 && review->Pos.y >= 0 &&
				review->Pos.x + review->Size.x <= ImGui::GetIO().DisplaySize.x &&
				review->Pos.y + review->Size.y <= ImGui::GetIO().DisplaySize.y,
			"Enlarged restart review exceeds the viewport");
	if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "headset-restart-review-enlarged.bmp");
		test.Frame();
	}
	process.process = 43;
	ImGui::ActivateItemByID(review->GetID("Restart"));
	test.Frame();
	test.Frame();
	Require(!editor.TakeRestartRequest(), "Changed process accepted stale restart review");
	ImGui::ActivateItemByID(review->GetID("Keep running"));
	test.Frame();
	test.Frame();
	Require(!editor.TakeRestartRequest(), "Keep running requested restart");
	activate("Restart with headset settings");
	eyes = std::array<HeadsetEyeSize, 2>{{{3000, 3000}, {3000, 3000}}};
	ImGui::ActivateItemByID(review->GetID("Restart"));
	test.Frame();
	test.Frame();
	const auto request = editor.TakeRestartRequest();
	Require(request && request->process == 43, "Confirmed restart lost original process identity");
	const auto environment =
		std::map<std::string, std::string>(request->profile.environment.begin(), request->profile.environment.end());
	Require(environment.at("SIMXR_WIDTH") == "2200" && environment.at("SIMXR_HEIGHT") == "2300" &&
				environment.at("KEEP") == "unchanged",
			"Restart changed reviewed dimensions or unrelated environment");
	Require(!editor.TakeRestartRequest() && !changed && settings.profiles.front() == profile,
			"Restart replayed a request or changed saved settings");
	process.restartPending = true;
	canRestart = false;
	eyes.reset();
	test.Frame();
	ImGui::SetScrollY(form, form->ScrollMax.y);
	test.Frame();
	test.Frame();
	activate("Cancel restart");
	Require(editor.TakeRestartCancellation() && !editor.TakeRestartCancellation(),
			"Pending restart cancellation was lost or repeated");
	Require(!form->ScrollbarX, "Restart controls overflow horizontally");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Restart editor produced Vulkan validation errors");
}

void CheckPhysicalHeadsetPage()
{
	Interaction test;
	PhysicalHeadsetPage page;
	std::string conflict;
	HeadsetHostSnapshot host;
	ObservedSetup game;
	game.appTrackersKnown = true;
	game.appBodyExtensions = std::array{false, false, false};
	bool available = true;
	std::optional<HeadsetPageRequest> request;
	float width = 400;
	test.extraDraw = [&] {
		ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({width, 620}, ImGuiCond_Always);
		ImGui::Begin("Headset QA", nullptr, ImGuiWindowFlags_NoSavedSettings);
		request = page.Draw(host, available, conflict, &game);
		Require(ImGui::GetCurrentWindow()->DC.CursorMaxPos.x <= ImGui::GetCurrentWindow()->InnerRect.Max.x + 1,
				"Physical headset controls overflow the inspector width");
		ImGui::End();
	};
	test.Frame();
	test.Frame();
	auto* window = ImGui::FindWindowByName("Headset QA");
	const auto activate = [&](const char* label) {
		ImGui::ActivateItemByID(ImHashStr(label, 0, ImHashStr("physical-headset", 0, window->ID)));
		test.Frame();
	};
	activate("Connect");
	Require(request && request->connect && request->options.runtimeManifest.empty() && !request->options.localEyeHeight,
			"Connect defaults to the system runtime without fabricating LOCAL calibration");
	Require(!page.BodyEnabled() && !request->options.body, "Body forwarding must default to Off");
	const auto selectBody = [&](int index, const char* label) {
		activate("##body-provider");
		test.Frame();
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Body provider menu did not open");
		const auto seed = ImHashData(&index, sizeof(index), popups.back().Window->ID);
		ImGui::ActivateItemByID(ImHashStr(label, 0, seed));
		test.Frame();
		test.Frame();
	};
	const std::array labels{"FB upper body", "META full body", "BD without arms", "BD full body"};
	for (int provider = 0; provider < 4; ++provider)
	{
		selectBody(provider + 1, labels[provider]);
		activate("Connect");
		Require(page.BodyEnabled() && request && request->options.body == static_cast<simxr::BodyJointSet>(provider),
				"Selected body joint set did not reach Connect");
	}
	selectBody(0, "Body: Off");
	activate("Connect");
	Require(!page.BodyEnabled() && request && !request->options.body, "Off retained the previous body provider");
	available = false;
	activate("Connect");
	Require(!request, "Unavailable target allowed connection");
	activate("Detect headset");
	Require(request && request->connect && request->options.discover && request->options.endpoint.empty() &&
				!request->options.takeOver && !request->options.alignment,
			"Targetless discovery claimed game input");
	host.phase = HeadsetHostPhase::Running;
	host.discovering = true;
	host.recommendedEyes = std::array<HeadsetEyeSize, 2>{{{2048, 2160}, {2200, 2300}}};
	activate("Re-align");
	Require(!request, "Discovery allowed input realignment");
	available = true;
	activate("Connect");
	Require(request && request->connect && !request->options.discover && !request->options.takeOver,
			"Detected headset cannot connect directly to the running game");
	available = false;
	activate("Stop detection");
	Require(request && !request->connect, "Discovery cannot be stopped without a game");
	host = {};

	available = true;
	activate("Calibrate LOCAL height");
	activate("Connect");
	Require(request && request->options.localEyeHeight == 1.7f, "Explicit LOCAL calibration was not forwarded");
	activate("Custom runtime");
	activate("Connect");
	Require(!request, "Custom runtime without a manifest allowed connection");
	activate("Custom runtime");
	conflict = "Headset input is owned by a rig (source 81).";
	activate("Take over and connect");
	Require(request && request->connect && request->options.takeOver,
			"Explicit takeover action must forward authorization");
	conflict.clear();
	activate("Connect");
	Require(request && !request->options.takeOver, "Normal connect must not retain previous takeover authorization");
	host.phase = HeadsetHostPhase::Running;
	activate("Re-align");
	Require(!request, "Re-align requires acknowledged input readiness");
	host.input = HeadsetInputState::Ready;
	activate("Re-align");
	Require(request && request->realign && !request->connect, "Re-align must request a distinct controlled reconnect");
	activate("Disconnect");
	Require(request && !request->connect, "Running host cannot be disconnected");
	host.phase = HeadsetHostPhase::Stopping;
	activate("Disconnect");
	Require(!request, "Stopping host accepted a duplicate request");
	host.phase = HeadsetHostPhase::Exited;
	selectBody(4, "BD full body");
	host.bodySupport[2] = false;
	host.phase = HeadsetHostPhase::Failed;
	host.error = "The selected runtime has no usable headset. Check its connection and retry.";
	conflict = "Headset input is owned by a rig (source 81).";
	for (const float scale : {1.f, 1.5f})
	{
		test.workspace.SetScale(scale);
		width = scale == 1 ? 400 : 320 * scale;
		test.Frame();
		test.Frame();
		if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
		{
			test.renderer.RequestScreenshot(directory /
											(scale == 1 ? "headset-controls.bmp" : "headset-controls-enlarged.bmp"));
			test.Frame();
		}
		ImGui::SetScrollY(window, window->ScrollMax.y);
		test.Frame();
		test.Frame();
		activate("Take over and connect");
		Require(request && request->options.takeOver && request->options.body == simxr::BodyJointSet::FullBodyBD,
				"Scrolled body takeover must remain reachable at every scale");
		if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
		{
			test.renderer.RequestScreenshot(
				directory / (scale == 1 ? "headset-body-takeover.bmp" : "headset-body-takeover-enlarged.bmp"));
			test.Frame();
		}
		ImGui::SetScrollY(window, 0);
		test.Frame();
		test.Frame();
	}
	conflict.clear();
	host.phase = HeadsetHostPhase::Running;
	host.error.clear();
	for (const float scale : {1.f, 1.5f})
	{
		test.workspace.SetScale(scale);
		width = scale == 1 ? 400 : 320 * scale;
		for (const auto mode : {HeadsetOutputState::Live, HeadsetOutputState::Inspection, HeadsetOutputState::Hidden,
								HeadsetOutputState::Waiting})
		{
			host.output = mode;
			host.input =
				mode == HeadsetOutputState::Inspection ? HeadsetInputState::Suspended : HeadsetInputState::Ready;
			test.Frame();
			test.Frame();
			if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
			{
				const auto name = "headset-output-" + std::to_string(static_cast<unsigned>(mode)) +
								  (scale == 1 ? "-normal.bmp" : "-enlarged.bmp");
				test.renderer.RequestScreenshot(directory / name);
				test.Frame();
			}
		}
	}
	Require(test.validation->errors == 0, "Physical headset page produced Vulkan validation errors");
}

void CheckArchiveRecordingUi()
{
	Require(std::getenv("SIMXR_COMPANION_SETTINGS"), "Recording QA needs isolated settings");
	Interaction test;
	Runtime engine{ExtensionRegistry{std::span<const ExtensionDefinition>{}}};
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
	create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	std::strcpy(create.applicationInfo.applicationName, "Archive recording UI");
	Require(engine.CreateInstance(&create, instance) == XR_SUCCESS &&
				engine.CreateSession(instance, session) == XR_SUCCESS,
			"Create archive UI session");
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::RealTime;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->Ready(config.epoch) == XR_SUCCESS && session->Begin(begin, config) == XR_SUCCESS,
			"Begin archive UI session");
	control::ControlService service{engine};
#ifdef _WIN32
	const auto endpoint =
		std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(GetCurrentProcessId()) + ".archive-ui";
#else
	const auto endpoint = (CompanionSettingsPath().parent_path() / "capture.sock").string();
#endif
	control::LocalControlServer server{service, endpoint};
	const auto game = engine.Game().Register(SIMXR_GAME_CAN_RESET);
	Require(game && engine.Game().Publish(game, "checkpoint:start"), "Publish UI SDK origin");
	RecordingPanel panel;
	panel.SetTarget({endpoint, 1, session->Snapshot().generation.value});
	test.extraDraw = [&] {
		ImGui::SetNextWindowPos({30, 50}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({500, 250}, ImGuiCond_Always);
		ImGui::Begin("Recording fixture");
		panel.Poll(true);
		if (ImGui::Button("Record"))
			panel.RequestStart();
		ImGui::SameLine();
		if (ImGui::Button("Stop"))
			panel.RequestStop();
		panel.DrawPopovers(true);
		ImGui::End();
	};
	std::uint64_t frameId = 0;
	std::string lastTake;
	for (const auto scale : {1.f, 1.5f})
	{
		test.workspace.SetScale(scale);
		test.Frame();
		ImGui::ActivateItemByID(FindPanel("Recording fixture")->GetID("Record"));
		test.Frame();
		Eventually(
			[&] {
				test.Frame();
				return panel.Snapshot().phase == RecordingCapturePhase::Capturing;
			},
			"UI capture did not start");
		++frameId;
		CommandTransaction edit{frameId,
								{CommandSourceKind::Manual, 987},
								{FrameId{frameId}, {}},
								CommandLatePolicy::Reject,
								{TakeControl{ControlChannel::Head}, SetHeadPose{PoseSample{}}}};
		Require(session->Commands().Submit(edit).result == CommandResult::Queued, "Queue UI recorded input");
		FramePrediction prediction;
		Require(session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS &&
					session->EndFrame(prediction) == XR_SUCCESS,
				"Apply UI recorded input");
		for (unsigned event = 0; event < 20; ++event)
			Require(engine.Game().Publish(
						game, "checkpoint:target event " + std::to_string(event) +
								  " · 日本語 · Recorded gameplay telemetry remains readable in a bounded panel."),
					"Publish UI SDK evidence");
		++frameId;
		CommandTransaction tail{frameId,
								{CommandSourceKind::Manual, 987},
								{FrameId{frameId}, {}},
								CommandLatePolicy::Reject,
								{SetHeadPose{PoseSample{}}}};
		Require(session->Commands().Submit(tail).result == CommandResult::Queued, "Queue second UI input boundary");
		Require(session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS &&
					session->EndFrame(prediction) == XR_SUCCESS,
				"Apply second UI input boundary");
		ImGui::ActivateItemByID(FindPanel("Recording fixture")->GetID("Stop"));
		test.Frame();
		Eventually(
			[&] {
				test.Frame();
				return !panel.Snapshot().busy;
			},
			"UI capture did not finish");
		test.Frame();
		Require(panel.Snapshot().phase == RecordingCapturePhase::Complete && panel.Snapshot().error.empty() &&
					panel.Snapshot().transactions == 2,
				"UI archive failed");
		const auto saved = panel.TakeSaved();
		if (saved)
			lastTake = *saved;
		Require(saved && std::filesystem::exists(Utf8Path(*saved) / "input.simxr"),
				"UI omitted saved archive notification");
		Require(
			ImGui::IsPopupOpen(FindPanel("Recording fixture")->GetID("Recorded take"), ImGuiPopupFlags_AnyPopupLevel),
			"Saved take popup missing");
		if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) /
											(scale == 1 ? "take-saved.bmp" : "take-saved-enlarged.bmp"));
			test.Frame();
			test.renderer.WaitIdle();
		}
		auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
		ImGui::ActivateItemByID(popup->GetID("Done"));
		test.Frame();
	}
	Require(test.validation->errors.load() == 0, "Archive UI Vulkan validation errors");
	test.extraDraw = {};
	test.workspace.SetScale(1);
	test.workspace.ShowLibrary(true);
	test.workspace.Select(TakeSel{lastTake});
	for (int frame = 0; frame < 30; ++frame)
		test.Frame();
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(directory) / "archive-library.bmp");
		test.Frame();
	}
	Eventually(
		[&] {
			test.Frame();
			const auto* events = FindPanel("SDK events");
			return events && events->Active;
		},
		"SDK evidence inspector did not load its journal page");
	for (const auto scale : {1.f, 1.5f})
	{
		test.workspace.SetScale(scale);
		test.Frame();
		ImGui::SetScrollY(FindPanel("Inspector"), FindPanel("Inspector")->ScrollMax.y);
		test.Frame();
		ImGui::SetScrollY(FindPanel("Inspector"), FindPanel("Inspector")->ScrollMax.y);
		test.Frame();
		if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) /
											(scale == 1 ? "sdk-evidence.bmp" : "sdk-evidence-enlarged.bmp"));
			test.Frame();
			test.renderer.WaitIdle();
		}
	}
	test.workspace.SetScale(1);
	test.Frame();
	ImGui::SetScrollY(FindPanel("Inspector"), FindPanel("Inspector")->ScrollMax.y);
	test.Frame();
	ImGui::ActivateItemByID(FindPanel("Inspector")->GetID("Next events"));
	test.Frame();
	Eventually(
		[&] {
			test.Frame();
			return FindPanel("SDK events")->ScrollMax.y < 2 * ImGui::GetFontSize();
		},
		"SDK inspector did not load the final four-event page");
	ImGui::ActivateItemByID(FindPanel("Inspector")->GetID("First events"));
	test.Frame();
	Eventually(
		[&] {
			test.Frame();
			return FindPanel("SDK events")->ScrollMax.y > 8 * ImGui::GetFontSize();
		},
		"SDK inspector did not return to its first page");
	const auto evidence = automation::ReadRecordingEvidence(Utf8Path(lastTake));
	Require(!evidence.entries.empty() && evidence.entries.front().first && evidence.entries.front().last,
			"UI SDK evidence has no surrounding recorded range");
	ImGui::ActivateItemByID(FindPanel("Inspector")->GetID("Use nearby range"));
	test.Frame();
	ImGui::SetScrollY(FindPanel("Inspector"), 0);
	test.Frame();
	test.renderer.WaitIdle();
	const auto editable = CompanionSettingsPath().parent_path() / "recordings" /
						  ("Range " + std::to_string(evidence.entries.front().first->value) + "-" +
						   std::to_string(evidence.entries.front().last->value) + ".luau");
	Require(!std::filesystem::exists(editable), "Range UI fixture needs a fresh output name");
	ImGui::ActivateItemByID(FindPanel("Inspector")->GetID("Export editable rig"));
	Eventually(
		[&] {
			test.Frame();
			const auto selected = std::get_if<TakeSel>(&test.workspace.State().selection);
			return selected && selected->id == PathText(editable);
		},
		"Archive range export did not select the editable rig");
	Require(std::filesystem::exists(editable) && !OpenSavedRecording(PathText(editable)).path.empty(),
			"Archive UI did not save a reopenable editable range");
	Require(test.validation->errors.load() == 0, "Archive range UI Vulkan validation errors");
	std::cout << "PASS archive recording controls, automatic persistence and normal/enlarged completion popup\n";
}
}
