#include "app/UiPch.h"
#include "Cases.h"
#include "simulation/TrackingSystem.h"
#include "app/services/PlayerRig.h"
#include "app/services/RuntimeConnection.h"
#include "app/services/RuntimeSession.h"
#include "app/services/RecordingController.h"
#include "app/services/RunningApps.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/AppPicker.h"
#include "app/widgets/HapticEvents.h"
#include "app/widgets/TimelineCanvas.h"
#include "app/widgets/Theme.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include <iostream>
#include <random>
#ifdef __linux__
#include <csignal>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/un.h>
#include <sys/wait.h>
#include <unistd.h>
extern char** environ;
#endif

namespace simxr::tests
{

void CheckVideoExport(Interaction& test)
{
	const auto directory = QaDirectory("SIMXR_UI_QA_DIR");
	Require(!directory.empty(), "Video QA needs an output directory");
	Eventually(
		[&]
		{
			test.Frame();
			return test.runtime.History().Size() >= 32;
		},
		"Video history did not arrive");
	test.runtime.Pause();
	Eventually(
		[&]
		{
			test.Frame();
			return test.runtime.Snapshot().manual && !test.runtime.Snapshot().operationPending;
		},
		"Video fixture did not pause");
	const auto& history = test.runtime.History();
	const auto first = history.At(history.Size() - 16), last = history.At(history.Size() - 1);
	test.workspace.Select(RangeSel{first.identity, last.identity, first.epoch});
	test.Frame();
	test.Frame();
	auto* inspector = FindPanel("Inspector");
	Require(inspector, "Video range inspector missing");
	const auto destination = directory / "range-video.mp4";
	ImGui::ActivateItemByID(inspector->GetID("##Video file"));
	ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	test.Frame();
	auto& io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiKey_A, true);
	test.Frame();
	io.AddKeyEvent(ImGuiKey_A, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	io.AddInputCharactersUTF8(PathText(destination).c_str());
	test.Frame();
	io.AddKeyEvent(ImGuiKey_Enter, true);
	test.Frame();
	io.AddKeyEvent(ImGuiKey_Enter, false);
	test.Frame();
	ImGui::ActivateItemByID(inspector->GetID("Save video"));
	test.Frame();
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!std::filesystem::exists(destination) && std::chrono::steady_clock::now() < deadline)
		test.Frame();
	Require(std::filesystem::exists(destination), "Video export did not save a file");
	Require(std::filesystem::file_size(destination) > 100, "Video file missing");
	Require(test.runtime.Snapshot().manual, "Saving video resumed the app");
	test.renderer.RequestScreenshot(directory / "video-saved.bmp");
	test.Frame();
	test.Frame();
	test.runtime.ResumeLive();
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Video UI Vulkan validation errors");
	std::cout << "PASS selected range saved as video through UI while paused\n";
}

void CheckPreviewHistory(Interaction& test)
{
	const auto& history = test.runtime.History();
	Eventually(
		[&]
		{
			test.Frame();
			std::size_t covered = 0;
			for (std::size_t index = 0; index < history.Size(); ++index)
				covered += (history.At(index).flags & HistoryPreview) != 0;
			return covered >= 80;
		},
		"Continuous runtime history did not populate the Images lane");
	test.runtime.Pause();
	Eventually(
		[&]
		{
			test.Frame();
			return test.runtime.Snapshot().manual && !test.runtime.Snapshot().operationPending;
		},
		"History fixture did not pause");
	for (int frame = 0; frame < 10; ++frame)
		test.Frame();
	std::vector<std::size_t> frames;
	for (std::size_t index = 0; index < history.Size(); ++index)
		if (history.At(index).flags & HistoryPreview)
			frames.push_back(index);
	Require(frames.size() >= 64, "Pause discarded preview history");
	std::vector<HistoryFrame> thumbnails;
	for (std::size_t index = 0; index < frames.size(); index += std::max<std::size_t>(1, frames.size() / 12))
		if (history.At(frames[index]).identity.frame >= 3)
			thumbnails.push_back(history.At(frames[index]));
	test.extraDraw = [&]
	{
		auto& previews = test.runtime.KeptPreviews();
		previews.BeginThumbnails();
		for (const auto& frame : thumbnails)
			previews.RequestThumbnail(frame.identity, frame.epoch);
	};
	Eventually([&]
	{
		test.Frame();
		for (const auto& frame : thumbnails)
		{
			const auto shot = test.runtime.KeptPreviews().Nearest(frame.identity, frame.epoch);
			if (!shot || shot->frame != frame.identity)
				return false;
		}
		return true;
	}, "Timeline thumbnails did not load across retained history");
	TimelineCanvas overview;
	overview.Fit();
	test.extraDraw = [&]
	{
		ImGui::SetNextWindowPos({20, 480}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({1200, 280}, ImGuiCond_Always);
		ImGui::Begin("Retained timeline thumbnails");
		overview.Begin(100, 0);
		overview.Draw(history, test.runtime.KeptPreviews(), {0, 0, 1, 1}, false);
		ImGui::End();
	};
	test.Frame();
	Eventually([&]
	{
		test.Frame();
		return !test.runtime.KeptPreviews().MissingThumbnail();
	}, "Fitted timeline thumbnails did not finish loading");
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / "timeline-thumbnails.bmp");
		test.Frame();
	}
	test.extraDraw = {};

	frames.erase(frames.end() - 32, frames.end());
	frames.erase(frames.begin(), frames.end() - 32);
	std::uint64_t previous = 0;
	std::optional<float> inspectorHeight;
	bool capturedLoading = false;
	for (const auto index : frames)
	{
		const auto identity = history.At(index).identity;
		Require(!previous || identity.frame == previous + 1, "Retained preview coverage skips frames");
		previous = identity.frame;
		test.runtime.LookAt(index);
		Eventually(
			[&]
			{
				test.Frame();
				const auto view = test.runtime.Snapshot();
				const auto* inspector = FindPanel("Inspector");
				const float height = inspector->DC.CursorMaxPos.y - inspector->DC.CursorStartPos.y;
				if (inspectorHeight)
					Require(std::abs(height - *inspectorHeight) < 1.f, "Inspector shifted while changing frames");
				if (!view.selectedReady && !capturedLoading)
					if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
					{
						test.renderer.RequestScreenshot(std::filesystem::path(qa) / "scrub-loading.bmp");
						capturedLoading = true;
					}
				if (view.selectedReady && view.selectedIdentity == identity)
					inspectorHeight = height;
				return view.selectedReady && view.selectedIdentity == identity;
			},
			"Exact historical frame did not become visible");
		Require(test.runtime.PreviewBytes() <= test.runtime.PreviewBudget(), "Preview history exceeded budget");
	}
	for (const auto index : frames)
	{
		test.runtime.LookAt(index);
		test.Frame();
	}
	const auto final = history.At(frames.back()).identity;
	Eventually(
		[&]
		{
			test.Frame();
			const auto view = test.runtime.Snapshot();
			return view.selectedReady && view.selectedIdentity == final;
		},
		"Rapid scrubbing selected a stale image");
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / "preview-history.bmp");
		test.Frame();
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors with continuous preview history");
	test.runtime.SetLive(true);
	test.runtime.ResumeLive();
	std::cout << "PASS consecutive historical images after pause and rapid scrub; zero Vulkan validation errors\n";
}

#ifdef __linux__

void CheckSnapshotRetention(Interaction& test)
{
	const auto wait = [&](const auto& ready, const char* failure)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		for (;;)
		{
			test.Frame();
			const auto state = test.runtime.Snapshot();
			if (ready(state))
				return;
			if (std::chrono::steady_clock::now() >= deadline)
				throw std::runtime_error(std::string(failure) + ": " + state.phase + "/" + state.progress + " " +
										 state.error);
		}
	};
	wait(
		[](const auto& state)
		{
			return !state.operationPending;
		},
		"Initial retention configuration");
	test.runtime.Command(simxr::control::CommandKind::Pause);
	wait(
		[](const auto& state)
		{
			return state.manual && !state.operationPending && state.selectedReady;
		},
		"Pause for retention");
	test.runtime.Command(simxr::control::CommandKind::Step);
	wait([](const auto& state)
	{
		return !state.operationPending && state.selectedReady && state.selectedCapture &&
			state.selectedImageCapture == state.selectedCapture;
	}, "Initial exact snapshot");
	const auto pinned = test.runtime.Snapshot().selectedCapture;
	Require(pinned != 0, "Pause did not retain a boundary snapshot");
	test.runtime.Command(simxr::control::CommandKind::PinSnapshot, pinned);
	wait(
		[](const auto& state)
		{
			return !state.operationPending && state.selectedPinned;
		},
		"Pin boundary snapshot");
	for (int index = 0; index < 200; ++index)
	{
		const auto previous = test.runtime.Snapshot().selectedCapture;
		const auto timeline = FindPanel("##Frame timeline");
		Require(timeline, "Retention timeline missing");
		ImGui::ActivateItemByID(timeline->GetID("Step forward"));
		test.Frame();
		wait(
			[&](const auto& state)
			{
				return !state.operationPending && state.selectedReady && state.selectedCapture != previous &&
					   state.selectedImageCapture == state.selectedCapture;
			},
			"Next frame did not retire and replace its snapshot");
		Require(test.runtime.Snapshot().phase != "failed", "Next frame failed under retention pressure");
		if ((index + 1) % 25 == 0)
			std::cout << "Retention QA " << index + 1 << "/200 frames\n" << std::flush;
	}
	const auto frames = test.runtime.TimelineFrames();
	const auto saved = std::find_if(frames.begin(), frames.end(),
									[&](const auto& frame)
									{
										return frame.capture == pinned;
									});
	Require(saved != frames.end(), "Pinned snapshot disappeared from the catalog");
	test.runtime.SelectFrame(saved->local);
	wait(
		[&](const auto& state)
		{
			return state.selectedReady && state.selectedImageCapture == pinned && state.selectedPinned;
		},
		"Pinned image did not survive 200 steps");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors during rolling retention");
	std::cout << "PASS 200 native Next frame actions with pinned snapshot surviving; normalReadback=0\n";
}

void CheckAppPicker(const char* executable)
{
	const auto directoryValue = std::getenv("SIMXR_LAUNCH_QA_DIRECTORY");
	Require(directoryValue != nullptr, "Picker QA requires a new private directory");
	const std::filesystem::path directory(directoryValue);
	Require(directory.is_absolute() && std::filesystem::create_directory(directory),
			"Picker QA directory must be new and absolute");
	const auto settingsPath = directory / "companion.settings";
	Require(::setenv("SIMXR_COMPANION_SETTINGS", settingsPath.c_str(), 1) == 0, "Set isolated settings path");
	Require(::setenv("SIMXR_DATA_DIR", directory.c_str(), 1) == 0 && ::unsetenv("SIMXR_CONTROL_SOCKET") == 0,
			"Isolate the socket directory");
	CompanionSettings settings;
	settings.profiles.push_back({"Broken path", "/nonexistent/simxr-app"});
	settings.activeProfile = "Broken path";
	SaveSettings(settingsPath, settings);

	const auto stale = directory / ("control." + std::to_string(::getpid()) + ".sock");
	{
		const int descriptor = ::socket(AF_UNIX, SOCK_STREAM, 0);
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::snprintf(address.sun_path, sizeof(address.sun_path), "%s", stale.c_str());
		Require(descriptor >= 0 && ::bind(descriptor, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0,
				"Create a stale socket");
		::close(descriptor);
	}
	Require(::setenv("SIMXR_RECENT_CUSTOM", "literal value ; $()", 1) == 0 &&
			::setenv("SIMXR_RECENT_EMPTY", "", 1) == 0, "Set launch environment fixtures");
	pid_t probe = 0;
	char* arguments[] = {const_cast<char*>(executable), nullptr};
	Require(::posix_spawn(&probe, executable, nullptr, nullptr, arguments, environ) == 0, "Start the orphan app");
	::unsetenv("SIMXR_RECENT_CUSTOM");
	::unsetenv("SIMXR_RECENT_EMPTY");
	struct StopProbe
	{
		pid_t pid;
		~StopProbe()
		{
			::kill(pid, SIGTERM);
			::waitpid(pid, nullptr, 0);
		}
	} stopProbe{probe};
	const auto endpoint = (directory / ("control." + std::to_string(probe) + ".sock")).string();
	Eventually([&] { return std::filesystem::is_socket(endpoint); }, "Orphan app exposed no control socket");

	Interaction test(true);
	const auto pump = [&](int count)
	{
		for (int index = 0; index < count; ++index)
			test.Frame();
	};
	const auto shot = [&](const char* name)
	{
		if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(qa) / name);
			pump(1);
		}
	};
	const auto openPicker = [&]
	{
		pump(2);
		const auto* toolbar = FindPanel("##Workspace toolbar");
		Require(toolbar != nullptr, "Top bar absent");
		test.Click({toolbar->Pos.x + 20, toolbar->Pos.y + ImGui::GetFrameHeight() / 2});
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "App picker did not open");
		pump(1);
		return popups.back().Window->ID;
	};
	const auto closeEditor = [&]
	{
		auto* editor = ImGui::FindWindowByName("Launch profiles");
		Require(editor != nullptr, "Profile editor did not open");
		ImGui::ActivateItemByID(ImHashStr("#CLOSE", 0, editor->ID));
		pump(3);
	};

	const auto running = DiscoverRunningApps();
	Require(running.size() == 1 && running.front().endpoint == endpoint && running.front().process == std::uint64_t(probe) &&
				running.front().name == "xr_probe_linux" && running.front().started,
			"Running did not list exactly the orphan app with its PID and start time");
	const auto alias = std::filesystem::path(endpoint).parent_path() / "control.18446744073709551615.sock";
	std::filesystem::create_symlink(endpoint, alias);
	const auto aliased = DiscoverRunningApps();
	std::filesystem::remove(alias);
	const auto foundAlias = std::find_if(aliased.begin(), aliased.end(),
		[&](const auto& app) { return app.endpoint == alias.string(); });
	Require(foundAlias != aliased.end() && foundAlias->process == std::uint64_t(probe) &&
		foundAlias->name == "xr_probe_linux", "Discovery trusted the socket filename instead of its peer");
	auto picker = openPicker();
	pump(10);
	Require(!test.runtime.Snapshot().connected, "Discovery attached to a running app");
	shot("picker-running.bmp");

	ImGui::ActivateItemByID(ImHashStr("Edit profiles", 0, picker));
	pump(4);
	shot("editor-invalid.bmp");
	closeEditor();

	picker = openPicker();
	ImGui::ActivateItemByID(ImHashStr("Connect", 0, ImHashStr(endpoint.c_str(), 0, picker)));
	Eventually(
		[&]
		{
			pump(1);
			const auto state = test.runtime.Snapshot();
			return state.connected && state.endpoint == endpoint && state.selectedReady;
		},
		"Connect did not attach the orphan app");
	const auto stored = LoadSettings(settingsPath);
	Require(stored.recent.size() == 1 && stored.recent.front().launch.executable == executable,
			"The connected app is not a recent entry");
	const auto& environment = stored.recent.front().launch.environment;
	Require(std::find(environment.begin(), environment.end(),
		std::pair<std::string, std::string>{"SIMXR_RECENT_CUSTOM", "literal value ; $()"}) != environment.end() &&
		std::find(environment.begin(), environment.end(),
		std::pair<std::string, std::string>{"SIMXR_RECENT_EMPTY", ""}) != environment.end(),
		"Recent launch dropped custom or empty environment entries");
	std::cout << "PASS orphan app listed under Running, not attached until Connect, then connected " << endpoint
			  << '\n';

	picker = openPicker();
	shot("picker-recent.bmp");
	const int first = 0;
	ImGui::ActivateItemByID(ImHashStr("Save as profile", 0, ImHashData(&first, sizeof(first), picker)));
	pump(4);
	shot("editor-from-recent.bmp");
	const auto saved = LoadSettings(settingsPath);
	Require(saved.profiles.size() == 2 && saved.profiles.back().executable == executable &&
				saved.profiles.back().name == "xr_probe_linux",
			"Save as profile did not store the recent app");
	Require(saved.profiles.back().environment == environment, "Save as profile changed the recent environment");
	closeEditor();
	{
		struct RestoreSettings
		{
			std::filesystem::path path, backup;
			bool moved = false;
			~RestoreSettings()
			{
				if (!moved)
					return;
				std::error_code error;
				std::filesystem::remove(path, error);
				std::filesystem::rename(backup, path, error);
			}
		} restore{settingsPath, directory / "companion.settings.before-failure"};
		std::filesystem::rename(restore.path, restore.backup);
		restore.moved = true;
		Require(std::filesystem::create_directory(settingsPath), "Could not block the settings file commit");
		picker = openPicker();
		ImGui::ActivateItemByID(ImHashStr("Save as profile", 0, ImHashData(&first, sizeof(first), picker)));
		pump(4);
		const auto* editor = ImGui::FindWindowByName("Launch profiles");
		Require(editor && editor->Active, "Failing Save as profile did not open its editor");
		const auto run = test.workspace.State().session.state;
		Require(test.runtime.Snapshot().connected &&
					(run == RunState::Running || run == RunState::WaitingForSession || run == RunState::Paused),
				"Settings save failure changed the connected workspace into a failed run");
		const auto* errorDetails = ImGui::FindWindowByName("Error details");
		Require(!errorDetails || !errorDetails->Active, "Settings save failure opened the launch-error modal");
		Require(std::filesystem::is_directory(settingsPath), "Settings failure fixture did not preserve its blocked path");
		const auto layoutPath = settingsPath.parent_path() / "companion-v2.ini";
		std::filesystem::remove(layoutPath);
		bool finishFailed = false;
		try
		{
			test.workspace.Finish();
		}
		catch (const std::exception&)
		{
			finishFailed = true;
		}
		Require(finishFailed && std::filesystem::is_regular_file(layoutPath),
				"Settings failure prevented independent layout persistence at shutdown");
		closeEditor();
	}
	Require(LoadSettings(settingsPath).profiles.size() == saved.profiles.size(),
			"Settings failure fixture did not restore the previous settings file");
	std::cout << "PASS settings commit failure preserves the connected run and reports no launch-error modal\n";

	AppPicker namedPicker;
	CompanionSettings namedSettings;
	for (const auto* name : {"Edit profiles", "Disconnect", "First###shared", "Second###shared"})
		namedSettings.profiles.push_back({name, executable});
	RuntimeView connectedView;
	connectedView.connected = true;
	AppPickerChoice namedChoice;
	bool openNamed = false;
	test.live = false;
	test.extraDraw = [&]
	{
		ImGui::Begin("Named profile picker");
		const auto choice =
			namedPicker.Draw(namedSettings, connectedView, "Apps", 0, "", std::exchange(openNamed, false), {});
		if (choice.launchProfile || choice.editProfiles || choice.disconnect)
			namedChoice = choice;
		ImGui::End();
	};
	const auto openNamedPicker = [&]
	{
		namedChoice = {};
		openNamed = true;
		pump(2);
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Named profile picker did not open");
		return popups.back().Window->ID;
	};
	for (int index = 0; index < static_cast<int>(namedSettings.profiles.size()); ++index)
	{
		const auto popup = openNamedPicker();
		const auto row = ImHashData(&index, sizeof(index), ImHashStr("Profiles", 0, popup));
		ImGui::ActivateItemByID(ImHashStr("##Launch profile", 0, row));
		pump(2);
		Require(namedChoice.launchProfile == index && !namedChoice.editProfiles && !namedChoice.disconnect,
				"Profile name changed its launch selection or activated a picker action");
	}
	for (const auto* action : {"Edit profiles", "Disconnect"})
	{
		ImGui::ActivateItemByID(ImHashStr(action, 0, openNamedPicker()));
		pump(2);
		Require(!namedChoice.launchProfile &&
					(std::string_view(action) == "Edit profiles" ? namedChoice.editProfiles : namedChoice.disconnect),
				"Picker action launched a profile with the same name");
	}
	test.extraDraw = {};
	test.live = true;
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors in the app picker");
	std::cout << "PASS recent app saved as profile \"" << saved.profiles.back().name << "\" in two clicks\n";
}

void CheckBodyRig(Interaction& test)
{
	IgnoreRealPointer();
	const auto target = test.runtime.Snapshot();
	ManualInputController observer;
	observer.SetTarget({target.endpoint, target.session, target.generation});
	const auto qa = std::getenv("SIMXR_UI_QA_DIR");
	const auto pump = [&](int count)
	{
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto observe = [&]
	{
		const auto previous = observer.Snapshot().observation;
		observer.Refresh();
		Eventually(
			[&]
			{
				pump(1);
				return observer.Snapshot().observation > previous;
			},
			"Body observation stalled");
		return ReadObservedSetup(observer.Snapshot().state);
	};
	const auto screenshot = [&](const std::string& name)
	{
		if (!qa)
			return;
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / (name + ".bmp"));
		pump(1);
	};
	const auto activate = [&](std::initializer_list<const char*> path)
	{
		const auto inspector = FindPanel("Inspector");
		Require(inspector, "Inspector missing");
		ImGuiID id = inspector->ID;
		for (const auto* part : path)
			id = ImHashStr(part, 0, id);
		ImGui::ActivateItemByID(id);
		pump(6);
	};
	const auto near = [](XrVector3f a, XrVector3f b, float tolerance)
	{
		return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z) < tolerance;
	};
	SDL_ShowWindow(test.window.Native());
	SDL_RaiseWindow(test.window.Native());
	test.window.Resize(1280, 800);
	test.workspace.SetScale(1);
	test.workspace.Select(DeviceSel{DevicePage::Head});
	pump(10);

	auto state = observe();
	for (const auto& tracker : state.values.tracking.trackers)
	{
		const auto joint = BodyModel::TrackerJoint(tracker.role);
		Require(tracker.anchor.value_or(joint) == joint && !tracker.poseOverride, "Tracker did not start on its joint");
	}
	const auto markers = test.runtime.Snapshot().markers;
	int trackerMarkers = 0;
	for (int device = 3; device < 7; ++device)
		trackerMarkers += markers[device].width > 0;
	std::cout << "Body QA appTrackers=" << state.appTrackers << " trackerMarkers=" << trackerMarkers << "\n";
	Require(state.appTrackers || trackerMarkers == 0, "Tracker markers shown although the app has no trackers");
	for (int slot = 0; slot < 4; ++slot)
		Require(state.boundTrackers[slot] || markers[slot + 3].width == 0, "Unbound tracker has a marker");
	if (std::getenv("SIMXR_PROBE_DEVICES"))
		Require(std::count(state.boundTrackers.begin(), state.boundTrackers.end(), true) == 1 && trackerMarkers == 1,
			"Probe's one bound tracker did not produce exactly one clickable marker");
	if (trackerMarkers)
	{
		int device = 3;
		while (markers[device].width == 0)
			++device;
		const auto before = state.values.tracking.trackers[device - 3];
		const auto& marker = markers[device];
		const ImVec2 point{marker.x + marker.width / 2, marker.y + marker.height / 2};
		for (const bool down : {true, false})
		{
			SDL_Event event{};
			event.type = SDL_EVENT_MOUSE_MOTION;
			event.motion.windowID = SDL_GetWindowID(test.window.Native());
			event.motion.which = TestMouse;
			event.motion.x = point.x;
			event.motion.y = point.y;
			SDL_PushEvent(&event);
			event = {};
			event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.windowID = SDL_GetWindowID(test.window.Native());
			event.button.which = TestMouse;
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = down;
			SDL_PushEvent(&event);
			pump(1);
		}
		pump(8);
		state = observe();
		const auto& after = state.values.tracking.trackers[device - 3];
		Require(after.anchor == before.anchor && after.poseOverride.has_value() == before.poseOverride.has_value(),
				"Clicking a tracker marker moved the tracker");
		const auto page = std::get_if<DeviceSel>(&test.workspace.State().selection);
		Require(page && page->page == DevicePage::Trackers, "Tracker marker did not select the tracker");
		screenshot("trackers-100");
		test.workspace.Select(DeviceSel{DevicePage::Head});
		pump(6);
	}

	std::vector<std::pair<std::string, ShellRect>> rings;
	if (qa)
	{
		const auto started = SDL_GetTicks();
		while (SDL_GetTicks() - started < 10000)
		{
			const auto name = "head-ring-" + std::to_string(rings.size());
			rings.emplace_back((std::filesystem::path(qa) / (name + ".bmp")).string(),
							   test.runtime.Snapshot().rigDevices[0]);
			screenshot(name);
			pump(15);
		}
		test.renderer.WaitIdle();
		int fewest = 1 << 30, most = 0;
		for (const auto& [path, head] : rings)
		{
			SDL_Surface* loaded = SDL_LoadBMP(path.c_str());
			Require(loaded, "Head ring capture missing");
			SDL_Surface* image = SDL_ConvertSurface(loaded, SDL_PIXELFORMAT_RGBA32);
			SDL_DestroySurface(loaded);
			int count = 0;
			for (int y = static_cast<int>(head.y) - 15; y <= static_cast<int>(head.y) + 15; ++y)
				for (int x = static_cast<int>(head.x) - 15; x <= static_cast<int>(head.x) + 15; ++x)
				{
					const float distance = std::hypot(x - head.x, y - head.y);
					if (x < 0 || y < 0 || x >= image->w || y >= image->h || distance < 10 || distance > 14)
						continue;
					const auto* pixel = static_cast<const std::uint8_t*>(image->pixels) + y * image->pitch + x * 4;
					count += pixel[0] > 180 && pixel[1] > 180 && pixel[2] > 180;
				}
			SDL_DestroySurface(image);
			fewest = std::min(fewest, count);
			most = std::max(most, count);
		}
		std::cout << "Body QA head ring captures=" << rings.size() << " ringPixels=" << fewest << ".." << most << "\n";
		Require(rings.size() >= 10 && fewest > 20 && fewest * 10 >= most * 8, "Head selection ring flickered");
		std::cout << "PASS head ring stable over 10 s\n";
	}

	test.workspace.Select(DeviceSel{DevicePage::Player});
	pump(6);
	const auto standing = observe();
	activate({"Body pose", "T-pose"});
	state = observe();
	const auto evaluate = [](const ObservedSetup& observed)
	{
		return TrackingSystem::Evaluate(RuntimeTime{Duration{1}}, observed.values.tracking,
										BodyModel{observed.values.proportions});
	};
	auto posed = evaluate(state);
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		const auto palm =
			posed.body.joints[static_cast<std::size_t>(hand ? BodyJoint::RightHandPalm : BodyJoint::LeftHandPalm)]
				.pose.position;
		const auto shoulder =
			posed.body.joints[static_cast<std::size_t>(hand ? BodyJoint::RightArmUpper : BodyJoint::LeftArmUpper)]
				.pose.position;
		std::cout << "Body QA T-pose hand " << hand << " controller=" << posed.controllers[hand].pose.pose.position.x
				  << "," << posed.controllers[hand].pose.pose.position.y << " palm=" << palm.x << "," << palm.y
				  << " shoulder=" << shoulder.x << "," << shoulder.y << "\n";
		Require(near(posed.controllers[hand].pose.pose.position, palm, .03f), "T-pose hand is not on its palm");
		Require(std::abs(palm.y - shoulder.y) < .05f && std::abs(palm.x - shoulder.x) > .4f,
				"T-pose arms are not out to the sides");
	}
	for (std::size_t slot = 0; slot < 4; ++slot)
		Require(near(posed.trackers[slot].pose.pose.position,
					 posed.body.joints[static_cast<std::size_t>(
										   BodyModel::TrackerJoint(state.values.tracking.trackers[slot].role))]
						 .pose.position,
					 .001f),
				"Tracker left its joint in T-pose");
	std::cout << "PASS T-pose posed skeleton, hands on palms and trackers on joints\n";
	screenshot("player-tpose-100");
	SDL_Event key{};
	key.type = SDL_EVENT_KEY_DOWN;
	key.key.windowID = SDL_GetWindowID(test.window.Native());
	key.key.key = SDLK_Z;
	key.key.scancode = SDL_SCANCODE_Z;
	key.key.mod = SDL_KMOD_LCTRL;
	key.key.down = true;
	SDL_PushEvent(&key);
	pump(1);
	key.type = SDL_EVENT_KEY_UP;
	key.key.down = false;
	SDL_PushEvent(&key);
	pump(8);
	state = observe();
	for (std::size_t hand = 0; hand < 2; ++hand)
		Require(near(state.values.tracking.controllers[hand].pose.pose.position,
					 standing.values.tracking.controllers[hand].pose.pose.position, .015f),
				"Ctrl+Z did not restore the hands");
	for (std::size_t joint = 0; joint < BodyJointCount; ++joint)
		Require(state.values.tracking.body.joints[joint].has_value() ==
					standing.values.tracking.body.joints[joint].has_value(),
				"Ctrl+Z did not restore the body joints");
	std::cout << "PASS Ctrl+Z undid the T-pose in one step\n";

	activate({"Body pose", "A-pose"});
	activate({"Reset all"});
	state = observe();
	const TrackingState defaults;
	for (std::size_t hand = 0; hand < 2; ++hand)
		Require(near(state.values.tracking.controllers[hand].pose.pose.position,
					 defaults.controllers[hand].pose.pose.position, .015f),
				"Reset all did not restore the hands");
	Require(state.values.tracking.body.preset == BodyPreset::Standing, "Reset all did not stand the body");
	std::cout << "PASS Reset all restored hands and body\n";

	if (qa)
	{
		for (const float scale : {1.0f, 1.5f})
		{
			const std::string suffix = scale == 1 ? "-100" : "-150";
			test.workspace.SetScale(scale);
			test.workspace.Select(DeviceSel{DevicePage::Left});
			pump(8);
			screenshot("left-hand" + suffix);
			test.workspace.Select(DeviceSel{DevicePage::Trackers});
			pump(8);
			screenshot("trackers" + suffix);
			test.workspace.Select(DeviceSel{DevicePage::Player});
			pump(8);
			screenshot("player" + suffix);
		}
		test.workspace.SetScale(1);
		test.renderer.WaitIdle();
	}
	Require(test.validation->errors.load() == 0, "Body rig validation errors");
}

void CheckSoak(Interaction& test, const std::filesystem::path& directory)
{
	Require(std::filesystem::path(test.runtime.Snapshot().endpoint).parent_path() == directory,
			"Soak must use its isolated launch endpoint");
	const auto seedText = std::getenv("SIMXR_SOAK_SEED");
	const auto seed = seedText ? static_cast<unsigned>(std::stoul(seedText)) : 51023U;
	const auto durationText = std::getenv("SIMXR_SOAK_SECONDS");
	const auto duration = std::chrono::seconds(durationText ? std::stoul(durationText) : 600);
	std::mt19937 random(seed);
	std::deque<std::string> recent;
	unsigned actions = 0;
	const auto trace = [&](const std::string& action)
	{
		recent.push_back(std::to_string(++actions) + " " + action);
		if (recent.size() > 20)
			recent.pop_front();
		std::ofstream output(directory / "soak-last-actions.txt");
		output << "seed=" << seed << '\n';
		for (const auto& entry : recent)
			output << entry << '\n';
	};
	const auto pump = [&]
	{
		for (int frame = 0; frame < 3; ++frame)
			test.Frame();
		Require(test.validation->errors.load() == 0, "Vulkan validation error during soak");
		Require(test.runtime.Snapshot().connected && test.runtime.Snapshot().sessionAvailable,
				"Soak lost its probe session");
	};
	const auto activate = [&](const char* region, const char* label)
	{
		trace(std::string(region) + ": " + label);
		if (auto* panel = FindPanel(region); panel && panel->Active)
			ImGui::ActivateItemByID(panel->GetID(label));
		pump();
	};
	const auto pointer = [&](ImVec2 point, std::optional<bool> down)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(test.window.Native());
		event.motion.which = TestMouse;
		event.motion.x = point.x;
		event.motion.y = point.y;
		Require(SDL_PushEvent(&event), "Push soak motion");
		if (down)
		{
			event = {};
			event.type = *down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.windowID = SDL_GetWindowID(test.window.Native());
			event.button.which = TestMouse;
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = *down;
			event.button.x = point.x;
			event.button.y = point.y;
			Require(SDL_PushEvent(&event), "Push soak button");
		}
		pump();
	};
	const auto drag = [&](const char* region)
	{
		if (auto* panel = FindPanel(region); panel && panel->Active)
		{
			const ImVec2 start{panel->Pos.x + panel->Size.x * (.2f + random() % 60 / 100.f),
							   panel->Pos.y + panel->Size.y * (.2f + random() % 60 / 100.f)};
			const ImVec2 end{start.x + int(random() % 81) - 40, start.y + int(random() % 61) - 30};
			trace(std::string(region) + " drag " + std::to_string(start.x) + "," + std::to_string(start.y) +
				  " -> " + std::to_string(end.x) + "," + std::to_string(end.y));
			pointer(start, true);
			pointer(end, std::nullopt);
			pointer(end, false);
		}
	};
	IgnoreRealPointer();
	ImGui::GetIO().ConfigDebugHighlightIdConflicts = true;
	const auto started = std::chrono::steady_clock::now();
	auto progress = started;
	std::cout << "Soak seed=" << seed << " seconds=" << duration.count() << std::endl;
	try
	{
		while (std::chrono::steady_clock::now() - started < duration)
		{
			ImGui::ClosePopupsExceptModals();
			const auto action = random() % 14;
			switch (action)
			{
			case 0:
			{
				static constexpr const char* pages[]{"Player", "Head", "Left", "Right", "Body", "Gaze", "Trackers"};
				const auto* tabs = FindPanel("Device tabs");
				activate(tabs && tabs->Active ? "Device tabs" : "Device rail", pages[random() % std::size(pages)]);
				break;
			}
			case 1:
				drag("Image area");
				break;
			case 2:
				drag("Inspector");
				break;
			case 3:
				drag("Timeline");
				break;
			case 4:
			{
				activate("Image controls", "View");
				const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
				if (!popups.empty() && popups.back().Window)
				{
					static constexpr const char* choices[]{"Left", "Right", "Both", "Fit", "1:1", "2:1", "4:1", "Pick region"};
					const auto* choice = choices[random() % std::size(choices)];
					trace(std::string("View: ") + choice);
					ImGui::ActivateItemByID(popups.back().Window->GetID(choice));
					pump();
					if (std::string_view(choice) == "Pick region")
						drag("Image area");
				}
				break;
			}
			case 5:
				activate("Timeline", test.runtime.Snapshot().manual ? "Continue" : "Pause");
				break;
			case 6:
				activate("Timeline", random() % 2 ? "Step back" : "Step forward");
				break;
			case 7:
				activate("Timeline", test.runtime.Snapshot().recording ? "Stop recording" : "Rec");
				break;
			case 8:
				activate("Timeline", "Library");
				break;
			case 9:
				activate("Top bar", "Focus");
				break;
			case 10:
				activate("Timeline divider tab", "##Edge");
				break;
			case 11:
				activate("Inspector divider tab", "##Edge");
				break;
			case 12:
			{
				const int width = 640 + int(random() % 17) * 80, height = 640 + int(random() % 6) * 80;
				trace("resize " + std::to_string(width) + "x" + std::to_string(height));
				test.window.Resize(width, height);
				pump();
				break;
			}
			case 13:
			{
				const float scale = random() % 2 ? 1.f : 1.5f;
				trace("scale " + std::to_string(scale));
				test.workspace.SetScale(scale);
				pump();
				break;
			}
			}
			pump();
			SDL_Delay(25);
			if (std::chrono::steady_clock::now() - progress > std::chrono::seconds(30))
			{
				std::cout << "Soak seed=" << seed << " actions=" << actions << std::endl;
				progress = std::chrono::steady_clock::now();
			}
		}
		test.renderer.WaitIdle();
		Require(test.validation->errors.load() == 0, "Soak teardown validation errors");
	}
	catch (...)
	{
		std::cerr << "Soak failure seed=" << seed << '\n';
		for (const auto& entry : recent)
			std::cerr << entry << '\n';
		throw;
	}
	SDL_SetEventFilter(nullptr, nullptr);
	std::cout << "PASS soak seed=" << seed << " seconds=" << duration.count() << " actions=" << actions << std::endl;
}

#endif

#ifdef __linux__
void CheckHeadsetRestartWorkflow(Interaction& test, const std::filesystem::path& directory)
{
	const auto original = test.runtime.Snapshot();
	const auto processFor = [&](const std::string& endpoint)
	{
		Require(std::filesystem::path(endpoint).parent_path() == directory,
				"Restart escaped its isolated endpoint directory");
		const auto name = std::filesystem::path(endpoint).filename().string();
		Require(name.starts_with("control.") && name.ends_with(".sock"), "Unexpected restart endpoint name");
		return static_cast<pid_t>(std::stoi(name.substr(8, name.size() - 13)));
	};
	struct ProbeCleanup
	{
		std::filesystem::path directory;
		std::vector<pid_t> processes;
		~ProbeCleanup()
		{
			try
			{
				for (const auto& entry : std::filesystem::directory_iterator(directory))
				{
					const auto name = entry.path().filename().string();
					if (name.starts_with("control.") && name.ends_with(".sock"))
						processes.push_back(static_cast<pid_t>(std::stoi(name.substr(8, name.size() - 13))));
				}
			}
			catch (...)
			{
			}
			std::ranges::sort(processes);
			processes.erase(std::unique(processes.begin(), processes.end()), processes.end());
			for (const auto process : processes)
				try
				{
					const auto profile = DescribeProcess(process);
					if (profile && std::filesystem::path(profile->executable).filename() == "xr_probe_linux" &&
						std::ranges::find(profile->environment,
										  std::pair{std::string("SIMXR_DATA_DIR"), directory.string()}) !=
							profile->environment.end())
					{
						::kill(process, SIGTERM);
						::kill(process, SIGCONT);
					}
				}
				catch (...)
				{
				}
		}
	} cleanup{directory, {processFor(original.endpoint)}};
	const auto pumpUntil = [&](auto condition, const char* message)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		do
		{
			test.Frame();
			Require(std::chrono::steady_clock::now() < deadline, message);
		} while (!condition());
	};
	const auto detect = [&]
	{
		HeadsetHostOptions options;
		options.executable = Utf8Path(SDL_GetBasePath()) / "headset_host_service_tests";
		options.discover = true;
		test.runtime.ConnectHeadset(std::move(options));
		pumpUntil(
			[&]
			{
				return test.runtime.HeadsetStatus().recommendedEyes.has_value();
			},
			"Restart fixture did not detect headset dimensions");
	};
	const auto reviewRestart = [&]
	{
		ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("##App picker"));
		test.Frame();
		test.Frame();
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Restart app picker did not open");
		ImGui::ActivateItemByID(popups.back().Window->GetID("Edit profiles"));
		test.Frame();
		test.Frame();
		auto* form = FindPanel("Profile form");
		Require(form && form->Active, "Restart launch profile did not open");
		ImGui::SetScrollY(form, form->ScrollMax.y);
		test.Frame();
		test.Frame();
		ImGui::ActivateItemByID(ImHashStr("Restart with headset settings", 0, ImHashStr("2", 0, form->ID)));
		test.Frame();
		test.Frame();
		auto* review = FindPanel("Restart application?");
		Require(review && review->Active, "Owned application restart review missing");
		ImGui::ActivateItemByID(review->GetID("Restart"));
		test.Frame();
	};
	detect();
	reviewRestart();
	test.runtime.Disconnect();
	test.Frame();
	test.Frame();
	pumpUntil(
		[&]
		{
			return test.runtime.HeadsetStatus().phase == HeadsetHostPhase::Exited;
		},
		"Cancelled restart did not stop its discovery host");
	Require(::kill(cleanup.processes.front(), 0) == 0, "Target change closed the original game");
	auto* error = FindPanel("Error details");
	Require(error && error->Active, "Target-change cancellation did not explain why restart stopped");
	ImGui::ActivateItemByID(error->GetID("Dismiss"));
	test.Frame();
	test.Frame();
	ImGui::ClosePopupToLevel(0, true);
	test.runtime.ConnectEndpoint(original.endpoint);
	pumpUntil(
		[&]
		{
			return test.runtime.Snapshot().selectedReady;
		},
		"Cancelled restart lost the original session");
	Require(test.runtime.Snapshot().session == original.session &&
				test.runtime.Snapshot().generation == original.generation,
			"Cancelled restart replaced the application session");
	detect();
	reviewRestart();
	pumpUntil(
		[&]
		{
			const auto state = test.runtime.Snapshot();
			if (state.endpoint == original.endpoint || !state.connected || !state.selectedReady)
				return false;
			cleanup.processes.push_back(processFor(state.endpoint));
			return true;
		},
		"Restart did not attach to replacement live pixels");
	const auto host = test.runtime.HeadsetStatus();
	Require(host.phase == HeadsetHostPhase::Exited && !host.forcedStop,
			"Replacement attached before clean headset shutdown");
	Require(::kill(cleanup.processes.front(), 0) != 0 && errno == ESRCH,
			"Replacement launched before original process exited");
	const auto replacement = DescribeProcess(cleanup.processes.back());
	Require(replacement.has_value(), "Replacement process could not be inspected");
	const auto environment =
		std::map<std::string, std::string>(replacement->environment.begin(), replacement->environment.end());
	Require(environment.at("SIMXR_WIDTH") == "2200" && environment.at("SIMXR_HEIGHT") == "2300",
			"Replacement did not receive reviewed headset dimensions");
	Require(test.runtime.Snapshot().selectedLayers == 2 && test.runtime.Snapshot().error.empty(),
			"Replacement stereo session is not healthy");
	ImGui::ClosePopupToLevel(0, true);
	test.renderer.RequestScreenshot(directory / "headset-restarted.bmp");
	test.Frame();
	const auto replacementEndpoint = test.runtime.Snapshot().endpoint;
	detect();
	const auto stoppedProcess = cleanup.processes.back();
	Require(::kill(stoppedProcess, SIGSTOP) == 0, "Cannot gate the isolated replacement's exit");
	reviewRestart();
	pumpUntil(
		[&]
		{
			std::ifstream status(std::filesystem::path("/proc") / std::to_string(stoppedProcess) / "status");
			for (std::string line; std::getline(status, line);)
				if (line.starts_with("ShdPnd:") || line.starts_with("SigPnd:"))
				{
					std::istringstream field(line.substr(line.find(':') + 1));
					std::uint64_t signals = 0;
					field >> std::hex >> signals;
					if ((signals & (std::uint64_t{1} << (SIGTERM - 1))) != 0)
						return true;
				}
			return false;
		},
		"Restart did not deliver a close request to the gated application");
	const auto changedEndpoint = (directory / "different-target.sock").string();
	test.runtime.ConnectEndpoint(changedEndpoint);
	test.Frame();
	test.Frame();
	error = FindPanel("Error details");
	Require(error && error->Active, "Queued restart target change was not reported");
	ImGui::ActivateItemByID(error->GetID("Dismiss"));
	test.Frame();
	test.Frame();
	ImGui::ClosePopupToLevel(0, true);
	Require(::kill(stoppedProcess, SIGCONT) == 0, "Cannot release isolated application exit gate");
	pumpUntil(
		[&]
		{
			return ::kill(stoppedProcess, 0) != 0 && errno == ESRCH;
		},
		"Cancelled restart did not observe original close completion");
	for (unsigned frame = 0; frame < 50; ++frame)
		test.Frame();
	Require(test.runtime.Endpoint() == changedEndpoint,
			"Cancelled restart attached a replacement over the selected endpoint");
	for (const auto& entry : std::filesystem::directory_iterator(directory))
	{
		const auto name = entry.path().filename().string();
		if (!name.starts_with("control.") || !name.ends_with(".sock"))
			continue;
		const auto profile = DescribeProcess(processFor(entry.path().string()));
		Require(!profile || std::ranges::find(profile->environment,
											  std::pair{std::string("SIMXR_DATA_DIR"), directory.string()}) ==
								profile->environment.end(),
				"Cancelled restart launched another isolated application after the close completed");
	}
	std::cout << "PASS target-change cancellation preserved " << original.endpoint << "; restart attached "
			  << replacementEndpoint << "; queued target change cancelled the next replacement\n";
}
#endif
}
