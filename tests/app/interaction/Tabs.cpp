#include "app/UiPch.h"
#include "Cases.h"
#include "app/shell/ApplicationTabs.h"
#include "app/services/RunningApps.h"
#include "app/services/RuntimeSession.h"
#include "app/services/RuntimeConnection.h"
#include "app/services/RecordingController.h"
#include "app/services/SimulationEdits.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/HapticEvents.h"
#include "control/FrameInspection.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "control/SimulationControl.h"
#include "runtime/Runtime.h"
#include "runtime/SimulationExtensions.h"
#include "simulation/CommandScheduler.h"
#include "simulation/TrackingSystem.h"
#include "graphics/backends/vulkan/VulkanResources.h"
#include "graphics/SnapshotBudget.h"
#include "math/Math.h"
#include <iostream>
#include <random>
#ifdef __linux__
#include <csignal>
#include <unistd.h>
#endif

namespace simxr::tests
{
static bool IsCompletePng(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	std::array<unsigned char, 8> signature{};
	std::array<unsigned char, 12> trailer{};
	input.read(reinterpret_cast<char*>(signature.data()), signature.size());
	input.seekg(-static_cast<std::streamoff>(trailer.size()), std::ios::end);
	input.read(reinterpret_cast<char*>(trailer.data()), trailer.size());
	return input && signature == std::array<unsigned char, 8>{137, 80, 78, 71, 13, 10, 26, 10} &&
		   trailer == std::array<unsigned char, 12>{0, 0, 0, 0, 73, 69, 78, 68, 174, 66, 96, 130};
}

#ifdef __linux__
int RunTabLaunchFixture()
{
	using namespace std::chrono_literals;
	const auto* data = std::getenv("SIMXR_DATA_DIR");
	Require(data && *data, "Launch fixture requires a private data directory");
	ExtensionRegistry extensions{SimulationExtensions()};
	Runtime engine{extensions};
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
	create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	std::strcpy(create.applicationInfo.applicationName, "Launched tab fixture");
	Require(engine.CreateInstance(&create, instance) == XR_SUCCESS &&
				engine.CreateSession(instance, session) == XR_SUCCESS,
			"Create launched tab fixture");
	FrameSchedulerConfig configuration;
	configuration.mode = ExecutionMode::Deterministic;
	configuration.epoch = RuntimeTime{1s};
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->Ready(configuration.epoch) == XR_SUCCESS && session->Begin(begin, configuration) == XR_SUCCESS,
			"Start launched tab fixture");
	FramePrediction frame;
	Require(session->WaitFrame(frame) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS &&
				session->EndFrame(frame) == XR_SUCCESS,
			"Publish launched tab fixture frame");
	control::ControlService service{engine};
	control::LocalControlServer server{
		service, (std::filesystem::path(data) / ("control." + std::to_string(::getpid()) + ".sock")).string()};
	for (;;)
		std::this_thread::sleep_for(100ms);
}

void CheckTabLaunches()
{
	const auto directory =
		std::filesystem::temp_directory_path() /
		("simxr-tab-launch-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(directory);
	const auto saveEnvironment = [](const char* name) -> std::optional<std::string> {
		const auto* value = std::getenv(name);
		return value ? std::optional<std::string>{value} : std::nullopt;
	};

	struct Cleanup
	{
		std::filesystem::path directory;
		std::optional<std::string> settings, data, endpoint;

		~Cleanup()
		{
			std::error_code ignored;
			for (std::filesystem::directory_iterator entry(directory, ignored), end; !ignored && entry != end;
				 entry.increment(ignored))
			{
				const auto name = entry->path().filename().string();
				if (!name.starts_with("control.") || !name.ends_with(".sock"))
					continue;
				try
				{
					const auto pid = std::stol(name.substr(8, name.size() - 13));
					if (pid > 1 && pid <= std::numeric_limits<pid_t>::max())
					{
						const auto process = DescribeProcess(static_cast<std::uint64_t>(pid));
						if (process && std::filesystem::equivalent(process->executable, "/proc/self/exe") &&
							std::ranges::find(process->environment,
											  std::pair{std::string("SIMXR_DATA_DIR"), directory.string()}) !=
								process->environment.end())
							::kill(static_cast<pid_t>(pid), SIGTERM);
					}
				}
				catch (const std::exception&)
				{}
			}
			const auto restore = [](const char* name, const std::optional<std::string>& value) {
				if (value)
					::setenv(name, value->c_str(), 1);
				else
					::unsetenv(name);
			};
			restore("SIMXR_COMPANION_SETTINGS", settings);
			restore("SIMXR_DATA_DIR", data);
			restore("SIMXR_CONTROL_SOCKET", endpoint);
			std::filesystem::remove_all(directory, ignored);
		}
	} cleanup{directory, saveEnvironment("SIMXR_COMPANION_SETTINGS"), saveEnvironment("SIMXR_DATA_DIR"),
			  saveEnvironment("SIMXR_CONTROL_SOCKET")};

	const auto settingsPath = directory / "settings.json";
	Require(::setenv("SIMXR_COMPANION_SETTINGS", settingsPath.c_str(), 1) == 0 &&
				::setenv("SIMXR_DATA_DIR", directory.c_str(), 1) == 0 && ::unsetenv("SIMXR_CONTROL_SOCKET") == 0,
			"Isolate launch fixture environment");
	CompanionSettings settings;
	LaunchProfile profile;
	profile.name = "Multiplayer client";
	profile.executable = std::filesystem::read_symlink("/proc/self/exe").string();
	profile.environment = {{"SIMXR_TAB_LAUNCH_FIXTURE", "1"}, {"SIMXR_DATA_DIR", directory.string()}};
	const auto script = directory / "background.luau";
	{
		std::ofstream output(script);
		output << "while true do end\n";
		Require(bool(output), "Cannot prepare background launch script");
	}
	profile.rig = script.string();
	settings.profiles.push_back(profile);
	settings.activeProfile = profile.name;
	SaveSettings(settingsPath, settings);
	Interaction test;
	{
		ApplicationTabs tabs{test.processes, test.renderer, test.window, true, "", true};
		const auto frame = [&] {
			Eventually([&] { return test.PollEvents() && test.renderer.BeginFrame(); }, "Launch tab UI stalled");
			tabs.Draw();
			Require(test.renderer.Render(tabs.ReadyWaits(),
										 [&](VkCommandBuffer command) { tabs.RecordTransfers(command); }),
					"Launch tab frame was not rendered");
			tabs.Submitted(test.renderer.SubmittedFrames());
		};
		const auto launch = [&] {
			frame();
			auto* toolbar = FindPanel("Top bar");
			Require(toolbar, "Launch picker toolbar missing");
			ImGui::ActivateItemByID(toolbar->GetID("##App picker"));
			frame();
			frame();
			const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
			Require(!popups.empty() && popups.back().Window, "Launch picker did not open");
			const int index = 0;
			const auto group = ImHashStr("Profiles", 0, popups.back().Window->ID);
			const auto row = ImHashData(&index, sizeof(index), group);
			ImGui::ActivateItemByID(ImHashStr("##Launch profile", 0, row));
			frame();
		};
		launch();
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"First launch did not attach to its own session");
		Eventually(
			[&] {
				frame();
				return tabs.ActiveWorkspace().HasActiveAutomation();
			},
			"First launch did not start its script");
		const auto first = tabs.ActiveRuntime().Endpoint();
		Require(std::filesystem::path(first).parent_path() == directory, "First launch escaped its fixture directory");
		launch();
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() != first && tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"Second launch of the same profile did not attach independently");
		const auto second = tabs.ActiveRuntime().Endpoint();
		const auto pid = [](const std::string& endpoint) {
			const auto name = std::filesystem::path(endpoint).filename().string();
			return static_cast<pid_t>(std::stol(name.substr(8, name.size() - 13)));
		};
		Require(std::filesystem::path(second).parent_path() == directory && tabs.Count() == 2,
				"The two launches did not retain separate tabs and endpoints");
		Eventually(
			[&] {
				frame();
				return tabs.ActiveWorkspace().HasActiveAutomation();
			},
			"Second launch did not start its independent script");
		tabs.OpenApplication(RunningApp{first});
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == first;
			},
			"Return to first launched player");
		Require(tabs.ActiveWorkspace().HasActiveAutomation(), "Background launch script stopped when switching tabs");
		tabs.ActiveWorkspace().StopAutomation();
		Eventually(
			[&] {
				frame();
				return !tabs.ActiveWorkspace().HasActiveAutomation();
			},
			"First script did not stop");
		tabs.OpenApplication(RunningApp{second});
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == second;
			},
			"Return to second launched player");
		Require(tabs.ActiveWorkspace().HasActiveAutomation(), "Stopping the first script stopped the second script");
		tabs.OpenApplication(RunningApp{first});
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == first;
			},
			"Select the first player for restart");
		auto& restarting = tabs.ActiveRuntime();
		auto& restartWorkspace = tabs.ActiveWorkspace();
		HeadsetHostOptions discovery;
		discovery.executable = Utf8Path(SDL_GetBasePath()) / "headset_host_service_tests";
		discovery.discover = true;
		restarting.ConnectHeadset(discovery);
		Eventually(
			[&] {
				frame();
				return restarting.HeadsetStatus().recommendedEyes.has_value();
			},
			"Restart tab did not discover headset dimensions");
		ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("##App picker"));
		frame();
		frame();
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Restart picker did not open");
		ImGui::ActivateItemByID(popups.back().Window->GetID("Edit profiles"));
		frame();
		frame();
		auto* form = FindPanel("Profile form");
		Require(form && form->Active, "Restart profile editor did not open");
		ImGui::SetScrollY(form, form->ScrollMax.y);
		frame();
		frame();
		ImGuiWindow* review = nullptr;
		for (int revision = 1; revision <= 8 && !review; ++revision)
		{
			const auto scope = std::to_string(revision);
			ImGui::ActivateItemByID(
				ImHashStr("Restart with headset settings", 0, ImHashStr(scope.c_str(), 0, form->ID)));
			frame();
			frame();
			if (auto* window = FindPanel("Restart application?"); window && window->Active)
				review = window;
		}
		Require(review && review->Active, "Restart confirmation did not open");
		ImGui::ActivateItemByID(review->GetID("Restart"));
		frame();
		ImGui::ClosePopupToLevel(0, true);
		tabs.OpenApplication(RunningApp{second});
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == second;
			},
			"Switch away during restart");
		Eventually(
			[&] {
				frame();
				return restarting.Endpoint() != first && restarting.Snapshot().sessionRunning;
			},
			"Background restart did not attach its replacement");
		const auto replacement = restarting.Endpoint();
		Require(pid(replacement) != pid(first) &&
					restartWorkspace.Process().process == static_cast<std::uint64_t>(pid(replacement)) &&
					::kill(pid(first), 0) != 0 && errno == ESRCH && ::kill(pid(second), 0) == 0 &&
					tabs.ActiveWorkspace().HasActiveAutomation(),
				"Restart changed the other application or its running script");
		tabs.ActiveWorkspace().StopAutomation();
		Eventually(
			[&] {
				frame();
				return !tabs.ActiveWorkspace().HasActiveAutomation();
			},
			"Second script did not stop");
		Require(pid(replacement) != pid(second) && ::kill(pid(replacement), 0) == 0 && ::kill(pid(second), 0) == 0,
				"Same-profile launches did not leave both applications running");
		::kill(pid(replacement), SIGTERM);
		::kill(pid(second), SIGTERM);
		Eventually(
			[&] {
				frame();
				return test.processes.Outstanding() == 0;
			},
			"Launch fixtures did not exit");
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Vulkan validation errors during same-profile launches");
	std::cout << "PASS independent launches and background restart of one of two applications\n";
}

void CheckGpuTabInput(const std::string& firstEndpoint, const std::string& secondEndpoint)
{
	Interaction test;
	{
		ApplicationTabs tabs{test.processes, test.renderer, test.window, false, "", true};
		const auto frame = [&] {
			Eventually([&] { return test.PollEvents() && test.renderer.BeginFrame(); }, "Input tab UI stalled");
			tabs.Draw();
			Require(test.renderer.Render(tabs.ReadyWaits(),
										 [&](VkCommandBuffer command) { tabs.RecordTransfers(command); }),
					"Input tab frame was not rendered");
			tabs.Submitted(test.renderer.SubmittedFrames());
		};
		const auto gate = [&](const std::string& endpoint, int number) {
			const auto directory = std::filesystem::path(endpoint).parent_path();
			const auto name = "rendering." + std::to_string(number);
			Eventually(
				[&] {
					frame();
					return std::filesystem::exists(directory / (name + ".ready"));
				},
				"Input producer did not reach its frame gate");
			std::ofstream release(directory / (name + ".continue"));
			Require(bool(release), "Cannot release input producer frame");
		};
		const RunningApp first{firstEndpoint, "Input player one"}, second{secondEndpoint, "Input player two"};
		tabs.OpenApplication(first);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"First input tab did not connect");
		const auto firstSession = tabs.ActiveRuntime().Snapshot();
		tabs.OpenApplication(second);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == secondEndpoint &&
					   tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"Second input tab did not connect");
		const auto secondSession = tabs.ActiveRuntime().Snapshot();
		for (int index = 3; index <= 6; ++index)
			for (const auto& app : {first, second})
				gate(app.endpoint, index);
		tabs.OpenApplication(first);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == firstEndpoint &&
					   tabs.ActiveRuntime().Snapshot().selectedReady;
			},
			"First input preview unavailable");
		ManualInputController firstObserver, secondObserver;
		firstObserver.SetTarget({firstEndpoint, firstSession.session, firstSession.generation});
		secondObserver.SetTarget({secondEndpoint, secondSession.session, secondSession.generation});
		const auto observed = [&](ManualInputController& observer) {
			const auto before = observer.Snapshot().observation;
			observer.Refresh();
			Eventually(
				[&] {
					frame();
					return observer.Snapshot().observation > before;
				},
				"Input observation stalled");
			return ReadObservedSetup(observer.Snapshot().state).values.input.controllers[1].primary;
		};
		const auto pendingInputs = [](const control::Message& state) {
			const auto* document = state.Find(control::tag::Document);
			Require(document, "Input status omitted its document");
			const auto decode = [](std::span<const std::byte> bytes) {
				auto decoded = control::DecodeFields(bytes);
				Require(decoded.error == control::DecodeError::None, "Malformed input status");
				control::Message result;
				result.fields = std::move(decoded.fields);
				return result;
			};
			const auto root = decode(document->value);
			Require(root.Find(3), "Input status omitted object data");
			for (const auto& field : decode(root.Find(3)->value).fields)
			{
				const auto member = decode(field.value);
				if (member.Find(1) && member.Find(1)->AsText() == "pending")
				{
					Require(member.Find(3) && member.Find(3)->AsU64(), "Invalid pending input count");
					return *member.Find(3)->AsU64() != 0;
				}
			}
			throw std::runtime_error("Input status omitted pending transactions");
		};
		int firstGate = 7, secondGate = 7;
		const auto applyButton = [&](ManualInputController& observer, const std::string& endpoint, int& nextGate,
									 bool pressed, const char* failure) {
			Eventually(
				[&] {
					frame();
					if (observed(observer) == pressed)
						return true;
					if (pendingInputs(observer.Snapshot().state))
					{
						Require(nextGate < 14, "Input handoff exhausted the producer frame budget");
						gate(endpoint, nextGate++);
						const auto boundary = std::filesystem::path(endpoint).parent_path() /
											  ("rendering." + std::to_string(nextGate) + ".ready");
						Eventually(
							[&] {
								frame();
								return std::filesystem::exists(boundary);
							},
							"Input producer did not finish an admitted frame");
					}
					return false;
				},
				failure);
		};
		SDL_VirtualJoystickDesc description{};
		SDL_INIT_INTERFACE(&description);
		description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
		description.naxes = SDL_GAMEPAD_AXIS_COUNT;
		description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
		description.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
		description.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
		description.name = "SimXR tab controls QA";
		const auto id = SDL_AttachVirtualJoystick(&description);
		Require(id, "Attach tab controls gamepad");

		struct Gamepad
		{
			SDL_JoystickID id;
			SDL_Joystick* joystick = nullptr;
			Window& window;

			~Gamepad()
			{
				if (joystick)
					SDL_CloseJoystick(joystick);
				SDL_DetachVirtualJoystick(id);
				window.PollEvents();
			}
		} pad{id, SDL_OpenJoystick(id), test.window};

		Require(pad.joystick, "Open tab controls gamepad");
		Require(SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768) &&
					SDL_SetJoystickVirtualAxis(pad.joystick, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768),
				"Set neutral gamepad triggers");
		Require(SDL_ShowWindow(test.window.Native()) && SDL_RaiseWindow(test.window.Native()),
				"Show tab controls window");
		Eventually(
			[&] {
				frame();
				return test.window.Focused();
			},
			"Tab controls window did not receive focus");
		Require(!observed(firstObserver) && !observed(secondObserver), "Input fixtures started with a held button");
		Require(SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, true),
				"Press virtual gamepad button");
		applyButton(firstObserver, firstEndpoint, firstGate, true, "Button did not reach the first application");
		Require(!observed(secondObserver), "Background application received the first application's button");
		tabs.OpenApplication(second);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == secondEndpoint &&
					   tabs.ActiveRuntime().Snapshot().selectedReady;
			},
			"Second input preview unavailable");
		applyButton(firstObserver, firstEndpoint, firstGate, false, "Switching tabs left the first button held");
		applyButton(secondObserver, secondEndpoint, secondGate, true,
					"Held button did not reach the selected application");
		Require(SDL_SetJoystickVirtualButton(pad.joystick, SDL_GAMEPAD_BUTTON_SOUTH, false),
				"Release virtual gamepad button");
		applyButton(secondObserver, secondEndpoint, secondGate, false, "Selected application's button did not release");
		Require(!observed(firstObserver), "Released button reappeared in the background application");
		Require(SDL_HideWindow(test.window.Native()), "Hide tab controls window");
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Vulkan validation errors during tab input handoff");
	std::cout << "PASS tab gamepad input ownership\n";
}

#endif

static void CheckReplayTabClose(ApplicationTabs& tabs, Interaction& test, RuntimePanels& replayRuntime,
								WorkspacePanels& replayWorkspace, const std::string& firstEndpoint,
								const std::string& secondEndpoint, const std::function<void()>& frame)
{
	ImGui::ClosePopupsExceptModals();
	frame();
	auto* window = FindPanel("Applications");
	Require(window, "Replay tab bar is missing");
	auto* bar = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("Applications"));
	Require(bar, "Replay tab bar state is missing");
	ImGuiID id = 0;
	ImVec2 hover{};
	for (auto& item : bar->Tabs)
		if (std::string_view(ImGui::TabBarGetTabName(bar, &item)).find("###application-2") != std::string_view::npos)
		{
			id = item.ID;
			hover = {bar->BarRect.Min.x + item.Offset + item.Width - ImGui::GetFontSize() * .5f - bar->FramePadding.x,
					 (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2};
		}
	Require(id, "Background replay tab is missing");
	SDL_Event motion{};
	motion.type = SDL_EVENT_MOUSE_MOTION;
	motion.motion.windowID = SDL_GetWindowID(test.window.Native());
	motion.motion.which = TestMouse;
	motion.motion.x = hover.x;
	motion.motion.y = hover.y;
	Require(SDL_PushEvent(&motion), "Cannot hover the background replay close button");
	frame();
	frame();
	const auto close = [&] {
		ImGui::ActivateItemByID(ImGui::GetIDWithSeed("#CLOSE", nullptr, id));
		frame();
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Closing a replay did not ask for confirmation");
		return popups.back().Window;
	};
	const auto count = tabs.Count();
	auto* confirm = close();
	ImGui::ActivateItemByID(confirm->GetID("Cancel"));
	frame();
	Require(tabs.Count() == count && tabs.ActiveRuntime().Endpoint() == secondEndpoint &&
				replayWorkspace.HasActiveAutomation() && replayRuntime.Snapshot().manual,
			"Cancelling background tab close changed the replay or selected application");
	confirm = close();
	ImGui::ActivateItemByID(confirm->GetID("Stop and close"));
	frame();
	RuntimeSession observer{test.renderer, firstEndpoint};
	Eventually(
		[&] {
			frame();
			observer.Poll(false, false);
			const auto view = observer.View();
			return tabs.Count() == count - 1 && view.connected && view.sessionRunning && !view.manual &&
				   tabs.ActiveRuntime().Endpoint() == secondEndpoint && tabs.ActiveRuntime().Snapshot().selectedReady;
		},
		"Closing a background replay did not release its application or preserve the foreground preview");
	std::cout << "PASS background replay tab close and independent foreground preview\n";
}

#ifdef __linux__
void CheckMultiGpuTabs(const std::string& firstEndpoint, const std::string& secondEndpoint)
{
	Interaction test;
	{
		ApplicationTabs tabs{test.processes, test.renderer, test.window, false, "", true};
		const auto frame = [&] {
			Eventually([&] { return test.PollEvents() && test.renderer.BeginFrame(); },
					   "GPU tab frame admission stalled");
			tabs.Draw();
			Require(test.renderer.Render(tabs.ReadyWaits(),
										 [&](VkCommandBuffer command) { tabs.RecordTransfers(command); }),
					"GPU tab frame was not rendered");
			tabs.Submitted(test.renderer.SubmittedFrames());
		};
		const RunningApp first{firstEndpoint, "GPU player one"}, second{secondEndpoint, "GPU player two"};
		std::vector<RunningApp> apps{first, second};
		if (const auto* third = std::getenv("SIMXR_TABS_GPU_THIRD"))
		{
			const auto* fourth = std::getenv("SIMXR_TABS_GPU_FOURTH");
			Require(fourth, "Four GPU tabs require both additional endpoints");
			apps.emplace_back(RunningApp{third, "GPU player three"});
			apps.emplace_back(RunningApp{fourth, "GPU player four"});
		}
		tabs.OpenApplication(first);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"First GPU tab did not connect");
		tabs.ActiveRuntime().StartRecording();
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Snapshot().recording;
			},
			"First GPU tab did not begin recording");
		tabs.OpenApplication(second);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == secondEndpoint &&
					   tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"Second GPU tab did not connect");
		for (std::size_t index = 2; index < apps.size(); ++index)
		{
			tabs.OpenApplication(apps[index]);
			Eventually(
				[&] {
					frame();
					return tabs.ActiveRuntime().Endpoint() == apps[index].endpoint &&
						   tabs.ActiveRuntime().Snapshot().sessionRunning;
				},
				"Additional GPU tab did not connect");
		}
		for (int index = 3; index <= 5; ++index)
		{
			for (const auto& app : apps)
			{
				const auto directory = std::filesystem::path(app.endpoint).parent_path();
				const auto gate = "rendering." + std::to_string(index);
				Eventually(
					[&] {
						frame();
						return std::filesystem::exists(directory / (gate + ".ready"));
					},
					"GPU producer did not reach its frame gate");
				std::ofstream release(directory / (gate + ".continue"));
				Require(bool(release), "Cannot release GPU frame gate");
			}
		}
		for (const auto& app : apps)
		{
			const auto boundary = std::filesystem::path(app.endpoint).parent_path() / "rendering.6.ready";
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			while (!std::filesystem::exists(boundary))
			{
				frame();
				Require(std::chrono::steady_clock::now() < deadline, "GPU producer did not finish frame 5");
			}
		}
		for (std::size_t index = 2; index < apps.size(); ++index)
		{
			tabs.OpenApplication(apps[index]);
			try
			{
				Eventually(
					[&] {
						frame();
						return tabs.ActiveRuntime().Endpoint() == apps[index].endpoint &&
							   tabs.ActiveRuntime().History().Size() >= 5 &&
							   tabs.ActiveRuntime().Snapshot().selectedReady &&
							   tabs.ActiveRuntime().Snapshot().selectedFrame >= 5;
					},
					"Additional GPU tab lost its background history or preview");
			}
			catch (...)
			{
				const auto view = tabs.ActiveRuntime().Snapshot();
				std::cerr << "GPU tab " << index << " history=" << tabs.ActiveRuntime().History().Size()
						  << " frame=" << view.selectedFrame << " ready=" << view.selectedReady
						  << " error=" << view.error << " notice=" << view.notice << '\n';
				throw;
			}
		}
		tabs.OpenApplication(second);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == secondEndpoint;
			},
			"Return to the second GPU tab");
		try
		{
			Eventually(
				[&] {
					frame();
					return tabs.ActiveRuntime().Snapshot().selectedReady &&
						   tabs.ActiveRuntime().Snapshot().selectedFrame >= 5;
				},
				"Second GPU preview unavailable");
		}
		catch (...)
		{
			const auto state = tabs.ActiveRuntime().Snapshot();
			std::cerr << "GPU preview state: endpoint=" << state.endpoint << " frame=" << state.selectedFrame
					  << " ready=" << state.selectedReady << " following=" << state.time.following
					  << " images=" << state.images << " error=" << state.error << " notice=" << state.notice << '\n';
			throw;
		}
		tabs.OpenApplication(first);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == firstEndpoint && tabs.ActiveRuntime().History().Size() >= 5;
			},
			"Background GPU history unavailable");
		tabs.ActiveRuntime().StopRecording();
		Eventually(
			[&] {
				frame();
				return !tabs.ActiveRuntime().Snapshot().recordingBusy &&
					   std::holds_alternative<TakeSel>(tabs.ActiveWorkspace().State().selection);
			},
			"GPU tab did not save its recording");
		const auto saved = std::get<TakeSel>(tabs.ActiveWorkspace().State().selection).id;
		auto& replayRuntime = tabs.ActiveRuntime();
		auto& replayWorkspace = tabs.ActiveWorkspace();
		Require(replayRuntime.CanReplay(), "GPU recording is not available for replay");
		replayRuntime.RequestReplay(OpenSavedRecording(saved));
		Eventually(
			[&] {
				frame();
				return replayWorkspace.State().timeline.playing && replayRuntime.Snapshot().manual;
			},
			"GPU recording did not enter replay");
		tabs.OpenApplication(second);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == secondEndpoint;
			},
			"Switch away from GPU replay");
		Require(replayWorkspace.State().timeline.playing && replayWorkspace.HasActiveAutomation() &&
					!tabs.ActiveRuntime().Snapshot().manual,
				"Background replay changed the other GPU application's pause state");
		if (std::getenv("SIMXR_TABS_REPLAY_CLOSE_QA"))
			CheckReplayTabClose(tabs, test, replayRuntime, replayWorkspace, firstEndpoint, secondEndpoint, frame);
		else
		{
			replayWorkspace.StopAutomation();
			Eventually(
				[&] {
					frame();
					return !replayWorkspace.HasActiveAutomation() && !replayRuntime.Snapshot().manual;
				},
				"Stopping background GPU replay did not release its frame control");
			tabs.OpenApplication(first);
			Eventually(
				[&] {
					frame();
					return tabs.ActiveRuntime().Endpoint() == firstEndpoint;
				},
				"Return after GPU replay");
			const auto& history = tabs.ActiveRuntime().History();
			const auto index = history.Size() - 2;
			const auto expected = history.At(index).identity.frame;
			tabs.ActiveRuntime().LookAt(index);
			Eventually(
				[&] {
					frame();
					const auto view = tabs.ActiveRuntime().Snapshot();
					return view.selectedReady && view.selectedFrame == expected;
				},
				"Background GPU frame did not import exactly");
			tabs.OpenApplication(second);
			Eventually(
				[&] {
					frame();
					return tabs.ActiveRuntime().Endpoint() == secondEndpoint &&
						   tabs.ActiveRuntime().Snapshot().selectedReady;
				},
				"Second GPU preview did not recover");
			tabs.OpenApplication(first);
			Eventually(
				[&] {
					frame();
					const auto view = tabs.ActiveRuntime().Snapshot();
					return tabs.ActiveRuntime().Endpoint() == firstEndpoint && view.selectedReady &&
						   view.selectedFrame == expected;
				},
				"First GPU selection did not survive tab switching");
			for (int index = 0; index < 6; ++index)
			{
				tabs.OpenApplication(second);
				Eventually(
					[&] {
						frame();
						return tabs.ActiveRuntime().Endpoint() == secondEndpoint &&
							   tabs.ActiveRuntime().Snapshot().selectedReady;
					},
					"Repeated switching lost the second GPU preview");
				tabs.OpenApplication(first);
				Eventually(
					[&] {
						frame();
						const auto view = tabs.ActiveRuntime().Snapshot();
						return tabs.ActiveRuntime().Endpoint() == firstEndpoint && view.selectedReady &&
							   view.selectedFrame == expected;
					},
					"Repeated switching lost the retained GPU preview");
			}
			if (const auto directory = QaDirectory("SIMXR_TABS_QA_DIR"); !directory.empty())
			{
				test.renderer.RequestScreenshot(directory / "tabs-1280.bmp");
				frame();
				test.window.Resize(800, 600);
				for (int index = 0; index < 4; ++index)
					frame();
				test.renderer.RequestScreenshot(directory / "tabs-800.bmp");
				frame();
			}
			ImGui::ClosePopupsExceptModals();
			frame();
			auto* tabWindow = FindPanel("Applications");
			Require(tabWindow, "GPU tab bar is missing");
			auto* bar = ImGui::GetCurrentContext()->TabBars.GetByKey(tabWindow->GetID("Applications"));
			Require(bar, "GPU tab bar state is missing");
			ImGuiID firstTab = 0;
			ImVec2 hover{};
			for (auto& item : bar->Tabs)
				if (std::string_view(ImGui::TabBarGetTabName(bar, &item)).find("###application-2") !=
					std::string_view::npos)
				{
					firstTab = item.ID;
					hover = {bar->BarRect.Min.x + item.Offset + item.Width - ImGui::GetFontSize() * .5f -
								 bar->FramePadding.x,
							 (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2};
				}
			Require(firstTab, "First GPU tab is missing");
			SDL_Event motion{};
			motion.type = SDL_EVENT_MOUSE_MOTION;
			motion.motion.windowID = SDL_GetWindowID(test.window.Native());
			motion.motion.which = TestMouse;
			motion.motion.x = hover.x;
			motion.motion.y = hover.y;
			Require(SDL_PushEvent(&motion), "Cannot hover the GPU tab close button");
			frame();
			frame();
			const auto exportPath = std::filesystem::path(firstEndpoint).parent_path() / "closing-export.png";
			Require(tabs.ActiveRuntime().RequestExport(exportPath.string()),
					"Cannot start GPU export before tab close");
			Require(tabs.ActiveRuntime().ImageExport().Busy(), "GPU export was not pending at tab close");
			const auto beforeClose = tabs.Count();
			ImGui::ActivateItemByID(ImGui::GetIDWithSeed("#CLOSE", nullptr, firstTab));
			frame();
			Eventually(
				[&] {
					frame();
					return tabs.Count() == beforeClose - 1;
				},
				"GPU tab did not close after its export");
			Require(IsCompletePng(exportPath), "Closing a tab interrupted its accepted GPU export");
			tabs.OpenApplication(second);
			Eventually(
				[&] {
					frame();
					return tabs.ActiveRuntime().Endpoint() == secondEndpoint &&
						   tabs.ActiveRuntime().Snapshot().selectedReady;
				},
				"Closing the exporting tab interrupted the other GPU application");
		}
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Vulkan validation errors with two GPU tabs");
	std::cout << "PASS native GPU tabs, background history and exact retained preview\n";
}

void CheckApplicationTabs()
{
	using namespace std::chrono_literals;

	struct Fixture
	{
		ExtensionRegistry extensions{SimulationExtensions()};
		Runtime engine{extensions};
		Runtime::InstanceRef instance;
		Runtime::SessionRef session;
		FrameSchedulerConfig configuration;
		control::ControlService service{engine};
		std::unique_ptr<control::LocalControlServer> server;
		std::string endpoint;

		Fixture(const std::string& suffix)
		{
			XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
			create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
			std::strcpy(create.applicationInfo.applicationName, "Tab fixture");
			Require(engine.CreateInstance(&create, instance) == XR_SUCCESS &&
						engine.CreateSession(instance, session) == XR_SUCCESS,
					"Create tab runtime");
			configuration.mode = ExecutionMode::Deterministic;
			configuration.epoch = RuntimeTime{1s};
			const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr,
										   XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
			Require(session->Ready(configuration.epoch) == XR_SUCCESS &&
						session->Begin(begin, configuration) == XR_SUCCESS,
					"Start tab runtime");
			endpoint = (std::filesystem::temp_directory_path() /
						("simxr-tabs-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) +
						 suffix + ".sock"))
						   .string();
			server = std::make_unique<control::LocalControlServer>(service, endpoint);
		}

		void Submit()
		{
			FramePrediction frame;
			Require(session->WaitFrame(frame) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS &&
						session->EndFrame(frame) == XR_SUCCESS,
					"Submit tab fixture frame");
		}
	};

	Fixture first{"-one"}, second{"-two"}, third{"-three"}, fourth{"-four"};
	first.Submit();
	second.Submit();
	third.Submit();
	fourth.Submit();
	const auto directory =
		std::filesystem::temp_directory_path() /
		("simxr-tabs-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
	std::filesystem::create_directories(directory);
	const auto* previous = std::getenv("SIMXR_COMPANION_SETTINGS");
	const std::optional<std::string> saved = previous ? std::optional<std::string>{previous} : std::nullopt;
	const auto* previousData = std::getenv("SIMXR_DATA_DIR");
	const std::optional<std::string> savedData = previousData ? std::optional<std::string>{previousData} : std::nullopt;
	const auto* previousEndpoint = std::getenv("SIMXR_CONTROL_SOCKET");
	const std::optional<std::string> savedEndpoint =
		previousEndpoint ? std::optional<std::string>{previousEndpoint} : std::nullopt;

	struct RestoreSettings
	{
		std::filesystem::path directory;
		std::optional<std::string> previous;
		std::optional<std::string> data, endpoint;

		~RestoreSettings()
		{
			if (previous)
				::setenv("SIMXR_COMPANION_SETTINGS", previous->c_str(), 1);
			else
				::unsetenv("SIMXR_COMPANION_SETTINGS");
			if (data)
				::setenv("SIMXR_DATA_DIR", data->c_str(), 1);
			else
				::unsetenv("SIMXR_DATA_DIR");
			if (endpoint)
				::setenv("SIMXR_CONTROL_SOCKET", endpoint->c_str(), 1);
			else
				::unsetenv("SIMXR_CONTROL_SOCKET");
			std::error_code ignored;
			std::filesystem::remove_all(directory, ignored);
		}
	} restore{directory, saved, savedData, savedEndpoint};

	Require(::setenv("SIMXR_COMPANION_SETTINGS", (directory / "settings.json").c_str(), 1) == 0,
			"Set isolated tab recording directory");
	Require(::setenv("SIMXR_DATA_DIR", directory.c_str(), 1) == 0 &&
				::setenv("SIMXR_CONTROL_SOCKET", second.endpoint.c_str(), 1) == 0,
			"Set isolated running-app discovery");
	Interaction test;
	{
		ApplicationTabs tabs{test.processes, test.renderer, test.window, false, "", true};
		const auto frame = [&] {
			Eventually([&] { return test.PollEvents() && test.renderer.BeginFrame(); },
					   "Tab UI frame admission stalled");
			tabs.Draw();
			Require(test.renderer.Render(tabs.ReadyWaits(),
										 [&](VkCommandBuffer command) { tabs.RecordTransfers(command); }),
					"Tab UI frame was not rendered");
			tabs.Submitted(test.renderer.SubmittedFrames());
		};
		const auto closeTab = [&](std::string_view suffix) {
			frame();
			auto* window = FindPanel("Applications");
			Require(window, "Application tab bar is missing");
			auto* bar = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("Applications"));
			Require(bar, "Application tab bar state is missing");
			ImVec2 hover{};
			bool found = false;
			for (auto& item : bar->Tabs)
				if (std::string_view(ImGui::TabBarGetTabName(bar, &item)).find(suffix) != std::string_view::npos)
				{
					found = true;
					hover = {bar->BarRect.Min.x + item.Offset + item.Width - ImGui::GetFontSize() * .5f -
								 bar->FramePadding.x,
							 (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2};
				}
			Require(found, "Application tab to close is missing");
			SDL_Event event{};
			event.type = SDL_EVENT_MOUSE_MOTION;
			event.motion.windowID = SDL_GetWindowID(test.window.Native());
			event.motion.which = TestMouse;
			event.motion.x = hover.x;
			event.motion.y = hover.y;
			Require(SDL_PushEvent(&event), "Cannot hover the application close button");
			frame();
			frame();
			event = {};
			event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
			event.button.windowID = SDL_GetWindowID(test.window.Native());
			event.button.which = TestMouse;
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = true;
			Require(SDL_PushEvent(&event), "Cannot press the application close button");
			frame();
			event.type = SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.down = false;
			Require(SDL_PushEvent(&event), "Cannot release the application close button");
			frame();
		};
		const RunningApp app1{first.endpoint, "Player one"}, app2{second.endpoint, "Player two"};
		tabs.OpenApplication(app1);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"First tab did not connect");
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().History().Size() != 0;
			},
			"First tab has no history");
		tabs.ActiveRuntime().StartRecording();
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Snapshot().recording;
			},
			"First tab did not start recording");
		closeTab("###application-2");
		const auto& closePopup = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!closePopup.empty() && closePopup.back().Window,
				"Closing a recording tab did not ask for confirmation");
		ImGui::ActivateItemByID(closePopup.back().Window->GetID("Cancel"));
		frame();
		Require(tabs.ActiveRuntime().Snapshot().recording, "Cancelling tab closure stopped its recording");
		auto* toolbar = FindPanel("Top bar");
		Require(toolbar, "Application picker toolbar is missing");
		ImGui::ActivateItemByID(toolbar->GetID("##App picker"));
		frame();
		frame();
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Application picker did not open");
		const auto connect = ImHashStr("Connect", 0, ImHashStr(second.endpoint.c_str(), 0, popups.back().Window->ID));
		Eventually(
			[&] {
				ImGui::ActivateItemByID(connect);
				frame();
				return tabs.ActiveRuntime().Endpoint() == second.endpoint;
			},
			"Picker did not open the discovered application");
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == second.endpoint &&
					   tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"Second tab did not connect");
		{
			std::atomic<std::uint32_t> delivered{0};
			std::atomic<unsigned> stops{0};

			struct RumbleState
			{
				std::atomic<std::uint32_t>& delivered;
				std::atomic<unsigned>& stops;
			} rumble{delivered, stops};

			SDL_VirtualJoystickDesc gamepad{};
			SDL_INIT_INTERFACE(&gamepad);
			gamepad.type = SDL_JOYSTICK_TYPE_GAMEPAD;
			gamepad.naxes = SDL_GAMEPAD_AXIS_COUNT;
			gamepad.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
			gamepad.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
			gamepad.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
			gamepad.name = "SimXR tab input QA";
			gamepad.userdata = &rumble;
			gamepad.Rumble = [](void* data, Uint16 left, Uint16 right) {
				auto& state = *static_cast<RumbleState*>(data);
				const auto previous = state.delivered.exchange((std::uint32_t(left) << 16) | right);
				if (previous && !left && !right)
					++state.stops;
				return true;
			};
			const auto joystick = SDL_AttachVirtualJoystick(&gamepad);
			Require(joystick, "Attach tab input test gamepad");

			struct Detach
			{
				SDL_JoystickID id;
				Window& window;

				~Detach()
				{
					if (id)
					{
						SDL_DetachVirtualJoystick(id);
						window.PollEvents();
					}
				}
			} detach{joystick, test.window};

			test.PollEvents();
			test.window.ReadGamepad(GamepadBindings{});
			Require(test.window.GamepadIdentity() == joystick, "Open tab input test gamepad");
			{
				RuntimePanels retiring{test.renderer, first.endpoint, &test.window};
				Require(test.window.CapturePointer(true), "Capture the foreground tab's pointer");
				Require(test.window.Rumble({10000, 20000}) && delivered.load() == ((10000u << 16) | 20000u),
						"Start the foreground tab's rumble");
				retiring.Poll(false);
				Require(SDL_GetWindowRelativeMouseMode(test.window.Native()) &&
							delivered.load() == ((10000u << 16) | 20000u),
						"A background session change disturbed foreground input");
			}
			Require(SDL_GetWindowRelativeMouseMode(test.window.Native()) &&
						delivered.load() == ((10000u << 16) | 20000u),
					"Closing a background runtime disturbed foreground input");
			tabs.OpenApplication(app1);
			frame();
			Require(!SDL_GetWindowRelativeMouseMode(test.window.Native()) && delivered.load() == 0,
					"Switching tabs did not release the previous tab's pointer and rumble");
			tabs.OpenApplication(app2);
			Eventually(
				[&] {
					frame();
					return tabs.ActiveRuntime().Endpoint() == second.endpoint;
				},
				"Return after foreground input switch");
			Require(SDL_ShowWindow(test.window.Native()) && SDL_RaiseWindow(test.window.Native()),
					"Show the tab haptic test window");
			Eventually(
				[&] {
					frame();
					return test.window.Focused();
				},
				"Tab haptic test window did not receive focus");
			const auto action = reinterpret_cast<XrAction>(1);
			const std::array left{HapticDevice::LeftController}, right{HapticDevice::RightController};
			XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION, nullptr, XR_INFINITE_DURATION, 160, .25f};
			Require(second.session->Haptics().Apply(left, action, XR_NULL_PATH, vibration, second.configuration.epoch,
													true) == XR_SUCCESS,
					"Send the foreground application's haptic event");
			Eventually(
				[&] {
					frame();
					return delivered.load() == (16384u << 16);
				},
				"Foreground application rumble did not reach the gamepad");
			vibration.amplitude = .75f;
			Require(first.session->Haptics().Apply(right, action, XR_NULL_PATH, vibration, first.configuration.epoch,
												   true) == XR_SUCCESS,
					"Send a background application's haptic event");
			for (int index = 0; index < 4; ++index)
				frame();
			Require(delivered.load() == (16384u << 16), "Background application drove the gamepad");
			const auto beforeSwitch = stops.load();
			tabs.OpenApplication(app1);
			Eventually(
				[&] {
					frame();
					return tabs.ActiveRuntime().Endpoint() == first.endpoint;
				},
				"Switch to the other haptic application");
			Require(stops.load() > beforeSwitch, "Tab switching did not stop the previous application's rumble");
			Eventually(
				[&] {
					frame();
					return delivered.load() == 49151u;
				},
				"New foreground application's rumble did not reach the gamepad");
			tabs.OpenApplication(app2);
			Eventually(
				[&] {
					frame();
					return tabs.ActiveRuntime().Endpoint() == second.endpoint;
				},
				"Restore the second tab after haptic test");
			Require(SDL_HideWindow(test.window.Native()), "Hide the tab haptic test window");
			Require(test.window.Rumble({}) && SDL_DetachVirtualJoystick(joystick), "Detach the tab input test gamepad");
			detach.id = 0;
			test.PollEvents();
		}
		tabs.ActiveRuntime().StartRecording();
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Snapshot().recording;
			},
			"Second tab did not start its independent recording");
		const auto count = tabs.Count();
		for (int index = 0; index < 4; ++index)
		{
			first.Submit();
			second.Submit();
			frame();
		}
		tabs.OpenApplication(app1);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == first.endpoint && tabs.ActiveRuntime().History().Size() >= 5;
			},
			"Background tab lost its frame history");
		Require(tabs.Count() == count, "Opening an existing application created a duplicate tab");
		Require(tabs.ActiveRuntime().Snapshot().recording, "Switching tabs stopped the background recording");
		tabs.ActiveRuntime().StopRecording();
		Eventually(
			[&] {
				frame();
				return !tabs.ActiveRuntime().Snapshot().recording && !tabs.ActiveRuntime().Snapshot().recordingBusy &&
					   std::holds_alternative<TakeSel>(tabs.ActiveWorkspace().State().selection);
			},
			"Background recording did not stop and save");
		tabs.ActiveRuntime().LookAt(0);
		const auto selected = tabs.ActiveRuntime().Snapshot().time.cursor;
		tabs.OpenApplication(app2);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == second.endpoint;
			},
			"Select second tab");
		Require(tabs.ActiveRuntime().Snapshot().recording, "Second recording stopped when the first one stopped");
		Require(tabs.ActiveRuntime().Snapshot().time.following, "Historical selection leaked into another tab");
		tabs.OpenApplication(app1);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == first.endpoint;
			},
			"Return to first tab");
		Require(!tabs.ActiveRuntime().Snapshot().time.following &&
					tabs.ActiveRuntime().Snapshot().time.cursor == selected,
				"Switching tabs discarded the historical cursor");
		ImGui::ClosePopupsExceptModals();
		closeTab("###application-3");
		const auto& closingPopup = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!closingPopup.empty() && closingPopup.back().Window,
				"Closing a background recording did not ask for confirmation");
		ImGui::ActivateItemByID(closingPopup.back().Window->GetID("Stop and close"));
		frame();
		Eventually(
			[&] {
				frame();
				return tabs.Count() == count - 1;
			},
			"Closing a recording tab did not stop and detach it");
		std::size_t takes = 0;
		for (const auto& entry : std::filesystem::directory_iterator(directory / "recordings"))
			if (entry.path().extension() == ".simxrtake")
				++takes;
		Require(takes == 2, "Independent recordings did not create two distinct archives");
		second.Submit();
		Require(tabs.ActiveRuntime().Endpoint() == first.endpoint,
				"Closing another tab changed the active application");
		const RunningApp app3{third.endpoint, "Player three"}, app4{fourth.endpoint, "Player four"};
		tabs.OpenApplication(app3);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == third.endpoint &&
					   tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"Third tab did not connect");
		tabs.OpenApplication(app4);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == fourth.endpoint &&
					   tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"Fourth tab did not connect");
		tabs.ActiveRuntime().Pause();
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Snapshot().manual;
			},
			"Fourth player did not pause");
		third.Submit();
		tabs.OpenApplication(app3);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == third.endpoint && tabs.ActiveRuntime().History().Size() >= 2;
			},
			"Third tab lost background history");
		Require(!tabs.ActiveRuntime().Snapshot().manual, "Pausing one player paused another player");
		tabs.OpenApplication(app4);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == fourth.endpoint;
			},
			"Return to fourth player");
		Require(tabs.ActiveRuntime().Snapshot().manual, "Paused state did not stay with the fourth player");
		tabs.ActiveRuntime().ResumeLive();
		Eventually(
			[&] {
				frame();
				return !tabs.ActiveRuntime().Snapshot().manual;
			},
			"Fourth player did not resume");
		HeadsetHostOptions discovery;
		discovery.executable =
			std::filesystem::read_symlink("/proc/self/exe").parent_path() / "headset_host_service_tests";
		discovery.discover = true;
		tabs.ActiveRuntime().ConnectHeadset(discovery);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().HeadsetStatus().phase == HeadsetHostPhase::Running;
			},
			"Fourth player did not start headset detection");
		tabs.OpenApplication(app3);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == third.endpoint;
			},
			"Return to third player");
		bool rejected = false;
		try
		{
			tabs.ActiveRuntime().ConnectHeadset(discovery);
		}
		catch (const std::runtime_error&)
		{
			rejected = true;
		}
		Require(rejected, "A second tab claimed the headset while the first was still using it");
		tabs.OpenApplication(app4);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == fourth.endpoint;
			},
			"Return to headset owner");
		Require(tabs.ActiveRuntime().HeadsetStatus().phase == HeadsetHostPhase::Running,
				"Switching tabs stopped the owning headset host");
		tabs.ActiveRuntime().DisconnectHeadset();
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().HeadsetStatus().phase == HeadsetHostPhase::Exited;
			},
			"Headset owner did not stop cleanly");
		tabs.OpenApplication(app3);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == third.endpoint;
			},
			"Return to third player");
		tabs.ActiveRuntime().ConnectHeadset(discovery);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().HeadsetStatus().phase == HeadsetHostPhase::Running;
			},
			"A different tab could not detect the released headset");
		tabs.ActiveRuntime().DisconnectHeadset();
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().HeadsetStatus().phase == HeadsetHostPhase::Exited;
			},
			"Second headset detection did not stop cleanly");
		Require(tabs.Count() == count + 1, "Opening four applications changed the wrong tab");
		tabs.OpenApplication(app1);
		Eventually(
			[&] {
				frame();
				return tabs.ActiveRuntime().Endpoint() == first.endpoint;
			},
			"Return to first player");
		Require(!tabs.ActiveRuntime().Snapshot().time.following &&
					tabs.ActiveRuntime().Snapshot().time.cursor == selected,
				"Other tabs changed the first player's inspection cursor");
		ImGui::ClosePopupsExceptModals();
		frame();
		auto* tabWindow = FindPanel("Applications");
		Require(tabWindow, "Application tabs disappeared");
		auto* bar = ImGui::GetCurrentContext()->TabBars.GetByKey(tabWindow->GetID("Applications"));
		Require(bar, "Application tab bar disappeared");
		const auto beforeAdd = tabs.Count();
		ImGui::ActivateItemByID(ImHashStr("+", 0, bar->ID));
		Eventually(
			[&] {
				frame();
				return tabs.Count() == beforeAdd + 1 && tabs.ActiveRuntime().Endpoint().empty();
			},
			"Add tab did not open a fresh application workspace");
		frame();
		const auto& addPopups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!addPopups.empty() && addPopups.back().Window, "Add tab did not open the application picker");
		const auto reconnect =
			ImHashStr("Connect", 0, ImHashStr(second.endpoint.c_str(), 0, addPopups.back().Window->ID));
		Eventually(
			[&] {
				ImGui::ActivateItemByID(reconnect);
				frame();
				return tabs.ActiveRuntime().Endpoint() == second.endpoint &&
					   tabs.ActiveRuntime().Snapshot().sessionRunning;
			},
			"Fresh tab did not connect the selected running application");
		Require(tabs.Count() == beforeAdd + 1, "Connecting from a fresh tab created another blank tab");
		tabs.ActiveWorkspace().SetScale(1.5f);
		test.window.Resize(800, 600);
		for (int index = 0; index < 4; ++index)
			frame();
		for (const auto* name : {"Applications", "Top bar", "Image area", "Timeline"})
		{
			const auto* panel = FindPanel(name);
			Require(panel && panel->Pos.x >= 0 && panel->Pos.x + panel->Size.x <= 801,
					"Application tabs or shell overflowed the narrow enlarged window");
		}
		const auto* header = FindPanel("Top bar");
		const auto* headerTabs = FindPanel("Applications");
		Require(headerTabs->ParentWindow == header && headerTabs->Pos.y >= header->Pos.y &&
					headerTabs->Pos.y + headerTabs->Size.y <= header->Pos.y + header->Size.y &&
					headerTabs->Pos.x + headerTabs->Size.x < header->Pos.x + header->Size.x,
				"Application tabs and actions do not share the compact top bar");
		if (const auto output = QaDirectory("SIMXR_TABS_QA_DIR"); !output.empty())
		{
			test.renderer.RequestScreenshot(output / "tabs-800-large.bmp");
			frame();
		}
		tabs.ActiveWorkspace().SetScale(1.f);
		test.window.Resize(1280, 720);
		for (int index = 0; index < 4; ++index)
			frame();
		if (const auto output = QaDirectory("SIMXR_TABS_QA_DIR"); !output.empty())
		{
			test.renderer.RequestScreenshot(output / "tabs-1280x720.bmp");
			frame();
		}
		test.window.Resize(1920, 1080);
		for (int index = 0; index < 4; ++index)
			frame();
		if (const auto output = QaDirectory("SIMXR_TABS_QA_DIR"); !output.empty())
		{
			test.renderer.RequestScreenshot(output / "tabs-1920.bmp");
			frame();
		}
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Vulkan validation errors during tab interaction");
	std::cout << "PASS independent tabs, background history and historical selection\n";
}

void CheckLiveHaptics()
{
	using namespace std::chrono_literals;
	std::atomic<std::uint32_t> delivered{0};
	Interaction test;
	SDL_VirtualJoystickDesc description{};
	SDL_INIT_INTERFACE(&description);
	description.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	description.naxes = SDL_GAMEPAD_AXIS_COUNT;
	description.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	description.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
	description.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
	description.name = "SimXR isolated haptic QA";
	description.userdata = &delivered;
	description.Rumble = [](void* data, Uint16 left, Uint16 right) {
		static_cast<std::atomic<std::uint32_t>*>(data)->store((std::uint32_t(left) << 16) | right);
		return true;
	};
	const auto joystick = SDL_AttachVirtualJoystick(&description);
	Require(joystick != 0, "Attach haptic virtual gamepad");

	struct Detach
	{
		SDL_JoystickID id;

		~Detach()
		{
			if (id)
				SDL_DetachVirtualJoystick(id);
		}
	} detach{joystick};

	test.PollEvents();
	test.window.ReadGamepad(GamepadBindings{});
	Require(test.window.GamepadIdentity() == joystick, "Open isolated virtual gamepad");
	ExtensionRegistry extensions{SimulationExtensions()};
	Runtime engine{extensions};
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
	create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	std::strcpy(create.applicationInfo.applicationName, "Live haptic QA");
	Require(engine.CreateInstance(&create, instance) == XR_SUCCESS &&
				engine.CreateSession(instance, session) == XR_SUCCESS,
			"Create isolated live haptic runtime");
	FrameSchedulerConfig configuration;
	configuration.mode = ExecutionMode::Deterministic;
	configuration.epoch = RuntimeTime{1s};
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->Ready(configuration.epoch) == XR_SUCCESS && session->Begin(begin, configuration) == XR_SUCCESS,
			"Start isolated haptic session");
	FramePrediction firstFrame;
	Require(session->WaitFrame(firstFrame) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS &&
				session->EndFrame(firstFrame) == XR_SUCCESS,
			"Publish the initial live haptic device configuration");
	control::ControlService service{engine};
	const auto endpoint =
		(std::filesystem::temp_directory_path() /
		 ("simxr-haptics-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + ".sock"))
			.string();
	control::LocalControlServer server{service, endpoint};
	RuntimeSession observed{test.renderer, endpoint};
	const auto poll = [&] {
		observed.Poll(false);
		test.PollEvents();
		return observed.Haptics().Recent().events.size();
	};
	Eventually(
		[&] {
			poll();
			return observed.Haptics().Fresh(HapticFeedback::Clock::now());
		},
		"Live haptic observation never connected");
	const std::array left{HapticDevice::LeftController}, right{HapticDevice::RightController};
	const auto action = reinterpret_cast<XrAction>(1);
	XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION, nullptr, XR_INFINITE_DURATION, 160, .25f};
	Require(session->Haptics().Apply(left, action, XR_NULL_PATH, vibration, configuration.epoch, true) == XR_SUCCESS,
			"Apply left application output");
	vibration.amplitude = .75f;
	Require(session->Haptics().Apply(right, action, XR_NULL_PATH, vibration, configuration.epoch, true) == XR_SUCCESS,
			"Apply right application output");
	Eventually([&] { return poll() == 2; }, "Both hands did not reach the live companion feed");
	Require(test.window.Rumble(observed.Haptics().Motors(HapticFeedback::Clock::now(), true),
							   observed.Haptics().Deadline()) &&
				delivered.load() == ((16384u << 16) | 49151u),
			"Live hand amplitudes did not reach the SDL motors");
	Eventually([&] { return delivered.load() == 0; }, "Watchdog must stop motors without SDL/UI event pumping");
	test.extraDraw = [&] {
		ImGui::SetNextWindowPos({430, 180}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({370, 260}, ImGuiCond_Always);
		ImGui::Begin("Live haptic output QA");
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);
		DrawHapticEvents(observed.Haptics(), 0, true);
		ImGui::End();
	};
	for (int frame = 0; frame < 3; ++frame)
		test.Frame();
	if (const auto screenshot = std::getenv("SIMXR_HAPTICS_QA_SCREENSHOT"))
		test.renderer.RequestScreenshot(screenshot);
	test.Frame();
	Require(session->Haptics().Stop(left, action, configuration.epoch, true) == XR_SUCCESS, "Stop left haptic output");
	Eventually(
		[&] {
			poll();
			return observed.Haptics().Recent().events.front().status == HapticStatus::Stopped;
		},
		"Live stop event did not reach companion");
	Require(test.window.Rumble(observed.Haptics().Motors(HapticFeedback::Clock::now(), true),
							   observed.Haptics().Deadline()) &&
				delivered.load() == 49151,
			"Stopping left output changed the right motor");
	SDL_Event focus{};
	focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
	focus.window.windowID = SDL_GetWindowID(test.window.Native());
	SDL_PushEvent(&focus);
	test.PollEvents();
	Require(delivered.load() == 0, "Window focus loss did not stop native rumble");
	observed.Haptics().Motors(HapticFeedback::Clock::now(), false);
	Eventually(
		[&] {
			poll();
			return observed.Setup() && observed.Setup()->frame == firstFrame.id.value &&
				   observed.Input().observation != 0;
		},
		"Live input observation did not become available");
	const auto connected = observed.View();
	Require(connected.sessionAvailable && connected.sessionRunning, "Failure fixture has no active session");
	observed.TakeScopeChanged();
	server.Close();
	Eventually(
		[&] {
			poll();
			return observed.Epoch() != connected.epoch;
		},
		"Failed transport did not retire its active epoch");
	const auto disconnected = observed.View();
	Require(!disconnected.connected && !disconnected.sessionAvailable && !disconnected.sessionRunning &&
				!disconnected.session && !disconnected.generation && !disconnected.latestFrame &&
				!disconnected.manual && !disconnected.operationPending && !disconnected.pending &&
				!disconnected.error.empty(),
			"Failed connection retained an active session or discarded its failure detail");
	Require(!disconnected.selectedReady && !disconnected.selectedCapture && !disconnected.selectedFrame &&
				!disconnected.selectedIdentity.frame && !disconnected.selectedReader &&
				!disconnected.time.cursor.frame && !observed.Preview().Selected() && !observed.Setup() &&
				!observed.Input().observation && observed.Haptics().Recent().events.empty() &&
				observed.TakeScopeChanged(),
			"Failed connection retained its selected preview, input, cursor or haptic state");
	poll();
	Require(observed.Epoch() == disconnected.epoch && !observed.TakeScopeChanged() &&
				observed.View().error == disconnected.error,
			"Failed connection repeatedly reset its scope or lost the saved diagnostic");
	control::LocalControlServer restarted{service, endpoint};
	observed.ConnectEndpoint(endpoint);
	Eventually(
		[&] {
			poll();
			const auto state = observed.View();
			return state.connected && state.sessionRunning && observed.Setup().has_value() &&
				   observed.Haptics().Fresh(HapticFeedback::Clock::now());
		},
		"Explicit reconnect did not observe the replacement endpoint");
	const auto reconnected = observed.View();
	Require(reconnected.epoch > disconnected.epoch && reconnected.session == connected.session &&
				reconnected.generation == connected.generation && reconnected.error.empty(),
			"Explicit reconnect reused the failed scope or preserved its failure");
	std::cout
		<< "PASS unexpected connection failure retires session/input/haptics and explicit reconnect uses a new epoch\n";
	observed.Disconnect();
	poll();
	Require(test.window.Rumble({}), "Stop output before gamepad removal");
	Require(test.window.Rumble({10000, 20000}), "Start output before unplug");
	Require(SDL_DetachVirtualJoystick(joystick), "Detach haptic gamepad");
	detach.id = 0;
	test.PollEvents();
	Require(!test.window.HasGamepad(), "Removed haptic gamepad remained selected");
	test.extraDraw = {};
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation error in live haptic UI workflow");
	std::cout << "PASS live runtime observation, hand motor mapping, independent stop, focus loss and haptic UI; zero "
				 "validation errors\n";
}
#endif

static void CheckTabSnapshotBudget(ApplicationTabs& tabs, Window& window, const std::vector<RunningApp>& apps,
								   const std::function<void()>& frame,
								   const std::function<void(const std::function<bool()>&, const char*)>& wait)
{
	Require(apps.size() >= 3, "Snapshot pressure needs at least three live applications");
	const auto selectHistorical = [&](const RunningApp& app) -> RuntimePanels& {
		tabs.OpenApplication(app);
		wait(
			[&] {
				return tabs.ActiveRuntime().Endpoint() == app.endpoint && tabs.ActiveRuntime().Snapshot().selectedReady;
			},
			"Snapshot pressure tab did not render");
		auto& runtime = tabs.ActiveRuntime();
		std::optional<std::size_t> selected;
		std::uint64_t expected = 0;
		wait(
			[&] {
				const auto view = runtime.Snapshot();
				if (!view.selectedReady || view.selectedReader)
					return false;
				const auto& history = runtime.History();
				for (std::size_t index = history.Size(); index-- > 0;)
					if (history.At(index).epoch == view.epoch && history.At(index).identity == view.selectedIdentity)
					{
						selected = index;
						expected = view.selectedFrame;
						return true;
					}
				return false;
			},
			"Snapshot pressure has no cached live frame");
		runtime.LookAt(*selected);
		try
		{
			wait(
				[&] {
					const auto view = runtime.Snapshot();
					return view.selectedReady && view.selectedReader && view.selectedFrame == expected;
				},
				"Snapshot pressure could not retain the selected frame");
		}
		catch (...)
		{
			const auto view = runtime.Snapshot();
			std::cerr << "Snapshot selection " << app.name << " expected=" << expected
					  << " actual=" << view.selectedFrame << " ready=" << view.selectedReady
					  << " reader=" << view.selectedReader << " notice=" << view.notice << " error=" << view.error
					  << '\n';
			throw;
		}
		return runtime;
	};
	auto& first = selectHistorical(apps[0]);
	auto& second = selectHistorical(apps[1]);
	ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("Settings"));
	frame();
	frame();
	auto* preferences = FindPanel("Preference contents");
	Require(preferences && preferences->Active, "Snapshot budget preferences did not open");
	const auto general = ImHashStr("General", 0, preferences->GetID("Preference pages"));
	const auto slider = ImHashStr("Snapshots (MiB)", 0, general);
	ImGui::ActivateItemByID(slider);
	ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	frame();
	auto& io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiKey_A, true);
	frame();
	io.AddKeyEvent(ImGuiKey_A, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	io.AddInputCharactersUTF8("128");
	frame();
	io.AddKeyEvent(ImGuiKey_Enter, true);
	frame();
	io.AddKeyEvent(ImGuiKey_Enter, false);
	wait([&] { return simxr::SnapshotBudget::MiB() == 128; }, "Snapshot budget did not shrink to 128 MiB");
	auto* settings = FindPanel("Workspace settings");
	Require(settings, "Snapshot budget preferences disappeared");
	ImGui::ActivateItemByID(settings->GetID("Close"));
	frame();
	const auto shrinkStart = std::chrono::steady_clock::now();
	bool reported = false;
	wait(
		[&] {
			if (!reported && std::chrono::steady_clock::now() - shrinkStart > std::chrono::seconds(2))
			{
				reported = true;
				std::cerr << "Snapshot charges after shrink: first=" << first.SnapshotChargeBytes()
						  << " second=" << second.SnapshotChargeBytes()
						  << " total=" << PreviewTexture::TotalSnapshotChargeBytes()
						  << " first reader=" << first.Snapshot().selectedReader
						  << " second reader=" << second.Snapshot().selectedReader << '\n';
			}
			return first.SnapshotChargeBytes() == (64ull << 20) && second.SnapshotChargeBytes() == (64ull << 20) &&
				   PreviewTexture::TotalSnapshotChargeBytes() == (128ull << 20);
		},
		"Shrinking the shared budget did not retain the two selected images");
	tabs.OpenApplication(apps[2]);
	wait(
		[&] {
			const auto state = tabs.ActiveRuntime().Snapshot();
			return tabs.ActiveRuntime().Endpoint() == apps[2].endpoint &&
				   state.notice.find("Snapshot memory is full") != std::string::npos;
		},
		"Third application did not report shared snapshot pressure");
	Require(!tabs.ActiveRuntime().Snapshot().selectedReady &&
				PreviewTexture::TotalSnapshotChargeBytes() == (128ull << 20),
			"Shared budget admitted an image beyond its charged capacity");
	frame();
	auto* tabWindow = FindPanel("Applications");
	Require(tabWindow, "Snapshot pressure tab bar is missing");
	auto* bar = ImGui::GetCurrentContext()->TabBars.GetByKey(tabWindow->GetID("Applications"));
	Require(bar, "Snapshot pressure tab bar state is missing");
	ImGuiID id = 0;
	ImVec2 hover{};
	for (auto& item : bar->Tabs)
		if (std::string_view(ImGui::TabBarGetTabName(bar, &item)).find("###application-2") != std::string_view::npos)
		{
			id = item.ID;
			hover = {bar->BarRect.Min.x + item.Offset + item.Width - ImGui::GetFontSize() * .5f - bar->FramePadding.x,
					 (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2};
		}
	Require(id, "Snapshot pressure first tab is missing");
	SDL_Event motion{};
	motion.type = SDL_EVENT_MOUSE_MOTION;
	motion.motion.windowID = SDL_GetWindowID(window.Native());
	motion.motion.which = TestMouse;
	motion.motion.x = hover.x;
	motion.motion.y = hover.y;
	Require(SDL_PushEvent(&motion), "Cannot hover the snapshot pressure close button");
	frame();
	frame();
	const auto beforeClose = tabs.Count();
	ImGui::ActivateItemByID(ImGui::GetIDWithSeed("#CLOSE", nullptr, id));
	frame();
	wait(
		[&] {
			return tabs.Count() == beforeClose - 1 && tabs.ActiveRuntime().Endpoint() == apps[2].endpoint &&
				   tabs.ActiveRuntime().Snapshot().selectedReady;
		},
		"Third application did not recover after a snapshot owner closed");
	Require(second.SnapshotChargeBytes() == (64ull << 20) &&
				PreviewTexture::TotalSnapshotChargeBytes() <= (128ull << 20),
			"Snapshot recovery lost the other selected image or exceeded the shared budget");
}

static void CheckPortableReplayClose(ApplicationTabs& tabs, Interaction& test, const std::vector<RunningApp>& apps,
									 const std::function<void()>& frame,
									 const std::function<void(const std::function<bool()>&, const char*)>& wait)
{
	tabs.OpenApplication(apps.front());
	wait(
		[&] {
			const auto view = tabs.ActiveRuntime().Snapshot();
			return tabs.ActiveRuntime().Endpoint() == apps.front().endpoint && view.sessionRunning &&
				   view.setup.has_value();
		},
		"Replay application is not ready to record");
	auto& runtime = tabs.ActiveRuntime();
	auto& workspace = tabs.ActiveWorkspace();
	runtime.StartRecording();
	wait([&] { return runtime.Snapshot().recording; }, "Replay recording did not start");
	const auto firstFrame = runtime.Snapshot().latestFrame;
	wait([&] { return runtime.Snapshot().latestFrame >= firstFrame + 120; },
		 "Replay recording did not span enough application frames");
	const auto setup = runtime.Snapshot().setup;
	Require(setup.has_value(), "Replay recording lost its device state");
	auto pose = setup->values.tracking.head;
	pose.pose.position.x += .01f;
	ManualTransaction edit;
	edit.commands = {simxr::TakeControl{simxr::ControlChannel::Head, false}, simxr::SetHeadPose{pose}};
	ManualInputController input;
	const auto target = runtime.Snapshot();
	input.SetTarget({target.endpoint, target.session, target.generation});
	wait(
		[&] {
			const auto state = input.Snapshot();
			Require(state.error.empty(), state.error.c_str());
			return state.observation != 0;
		},
		"Replay input worker did not observe the session");
	Require(input.Submit(edit), "Replay input was not accepted");
	try
	{
		wait(
			[&] {
				const auto view = runtime.Snapshot();
				const auto state = input.Snapshot();
				Require(state.error.empty(), state.error.c_str());
				return view.recordedChanges > 0 && view.setup &&
					   std::abs(view.setup->values.tracking.head.pose.position.x - pose.pose.position.x) < .001f;
			},
			"Replay recording did not capture an applied input");
	}
	catch (...)
	{
		const auto view = runtime.Snapshot();
		const auto state = input.Snapshot();
		std::cerr << "Replay input: changes=" << view.recordedChanges << " expected=" << pose.pose.position.x
				  << " actual=" << (view.setup ? view.setup->values.tracking.head.pose.position.x : 0)
				  << " pending=" << state.editPending << " error=" << state.error << '\n';
		throw;
	}
	Require(input.Submit(ReleaseManualInputs{}), "Replay input release was not accepted");
	wait(
		[&] {
			const auto state = input.Snapshot();
			Require(state.error.empty(), state.error.c_str());
			return !state.editPending;
		},
		"Replay input release did not complete");
	runtime.StopRecording();
	wait(
		[&] {
			return !runtime.Snapshot().recordingBusy && std::holds_alternative<TakeSel>(workspace.State().selection);
		},
		"Replay recording was not saved");
	const auto saved = std::get<TakeSel>(workspace.State().selection).id;
	ImGui::ClosePopupsExceptModals();
	runtime.RequestReplay(OpenSavedRecording(saved));
	wait([&] { return workspace.State().timeline.playing && runtime.Snapshot().manual; },
		 "Saved recording did not enter replay");
	auto* timeline = FindPanel("Timeline");
	Require(timeline, "Replay timeline is missing");
	ImGui::ActivateItemByID(timeline->GetID("Pause"));
	frame();
	wait([&] { return workspace.State().timeline.playing && workspace.State().timeline.playbackPaused; },
		 "Saved replay did not pause for the close check");
	tabs.OpenApplication(apps[1]);
	wait(
		[&] {
			return tabs.ActiveRuntime().Endpoint() == apps[1].endpoint && tabs.ActiveRuntime().Snapshot().selectedReady;
		},
		"Foreground application lost its preview");
	CheckReplayTabClose(tabs, test, runtime, workspace, apps.front().endpoint, apps[1].endpoint, frame);
}

void CheckPortableLiveTabs(const std::vector<std::string>& endpoints, const std::filesystem::path& directory)
{
	Require(endpoints.size() == 2 || endpoints.size() == 4, "Live tab test requires two or four endpoints");
	Require(std::filesystem::is_directory(directory), "Live tab test requires an existing output directory");
	Interaction test;
	{
		ApplicationTabs tabs{test.processes, test.renderer, test.window, false, "", true};
		const auto frame = [&] {
			Eventually([&] { return test.PollEvents() && test.renderer.BeginFrame(); }, "Live tab UI stalled");
			tabs.Draw();
			Require(test.renderer.Render(tabs.ReadyWaits(),
										 [&](VkCommandBuffer command) { tabs.RecordTransfers(command); }),
					"Live tab frame was not rendered");
			tabs.Submitted(test.renderer.SubmittedFrames());
		};
		const auto wait = [&](auto condition, const char* message) {
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
			while (true)
			{
				frame();
				if (condition())
					return;
				Require(std::chrono::steady_clock::now() < deadline, message);
			}
		};
		std::vector<RunningApp> apps;
		for (std::size_t index = 0; index < endpoints.size(); ++index)
		{
			apps.push_back({endpoints[index], "Live player " + std::to_string(index + 1)});
			tabs.OpenApplication(apps.back());
			wait(
				[&] {
					const auto view = tabs.ActiveRuntime().Snapshot();
					return tabs.ActiveRuntime().Endpoint() == endpoints[index] && view.sessionRunning;
				},
				"Live application did not connect");
		}
		if (std::getenv("SIMXR_TABS_LIVE_GATE"))
			for (int index = 3; index <= 5; ++index)
				for (const auto& app : apps)
				{
					const auto root = std::filesystem::path(app.endpoint).parent_path();
					const auto name = "rendering." + std::to_string(index);
					wait([&] { return std::filesystem::exists(root / (name + ".ready")); },
						 "Live producer did not reach its frame gate");
					std::ofstream release(root / (name + ".continue"));
					Require(bool(release), "Cannot release live producer frame");
				}
		for (int pass = 0; pass < 3; ++pass)
			for (const auto& app : apps)
			{
				tabs.OpenApplication(app);
				wait(
					[&] {
						return tabs.ActiveRuntime().Endpoint() == app.endpoint &&
							   tabs.ActiveRuntime().Snapshot().selectedReady &&
							   tabs.ActiveRuntime().History().Size() >= 2;
					},
					"Switching lost a live preview or background history");
			}
		if (std::getenv("SIMXR_TABS_BUDGET_QA"))
			CheckTabSnapshotBudget(tabs, test.window, apps, frame, wait);
		else if (std::getenv("SIMXR_TABS_PORTABLE_REPLAY_QA"))
			CheckPortableReplayClose(tabs, test, apps, frame, wait);
		else
		{
			tabs.OpenApplication(apps.front());
			wait([&] { return tabs.ActiveRuntime().Endpoint() == endpoints.front(); }, "Select first live tab");
			tabs.ActiveRuntime().Pause();
			wait([&] { return tabs.ActiveRuntime().Snapshot().manual; }, "First live application did not pause");
			tabs.OpenApplication(apps[1]);
			wait(
				[&] {
					return tabs.ActiveRuntime().Endpoint() == endpoints[1] &&
						   tabs.ActiveRuntime().Snapshot().selectedReady;
				},
				"Second live tab lost its preview");
			Require(!tabs.ActiveRuntime().Snapshot().manual, "Pausing one live application paused another");
			tabs.OpenApplication(apps.front());
			wait([&] { return tabs.ActiveRuntime().Endpoint() == endpoints.front(); }, "Return to paused live tab");
			Require(tabs.ActiveRuntime().Snapshot().manual, "Paused state did not stay with its live application");
			tabs.ActiveRuntime().ResumeLive();
			wait(
				[&] {
					return !tabs.ActiveRuntime().Snapshot().manual && tabs.ActiveRuntime().Snapshot().selectedReady;
				},
				"First live application did not resume");
			std::optional<std::size_t> historyIndex;
			std::uint64_t expectedFrame = 0;
			wait(
				[&] {
					const auto view = tabs.ActiveRuntime().Snapshot();
					if (!view.selectedReady || view.selectedReader)
						return false;
					const auto& history = tabs.ActiveRuntime().History();
					for (std::size_t index = history.Size(); index-- > 0;)
						if (history.At(index).epoch == view.epoch &&
							history.At(index).identity == view.selectedIdentity)
						{
							historyIndex = index;
							expectedFrame = view.selectedFrame;
							return true;
						}
					return false;
				},
				"Live image was not cached after releasing its reader");
			tabs.ActiveRuntime().LookAt(*historyIndex);
			wait(
				[&] {
					const auto view = tabs.ActiveRuntime().Snapshot();
					return view.selectedReady && view.selectedReader && view.selectedFrame == expectedFrame;
				},
				"Historical live image is not available for export");
			ImGui::ClosePopupsExceptModals();
			frame();
			auto* window = FindPanel("Applications");
			Require(window, "Live application tab bar is missing");
			auto* bar = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("Applications"));
			Require(bar, "Live application tab bar state is missing");
			ImGuiID id = 0;
			ImVec2 hover{};
			for (auto& item : bar->Tabs)
				if (std::string_view(ImGui::TabBarGetTabName(bar, &item)).find("###application-2") !=
					std::string_view::npos)
				{
					id = item.ID;
					hover = {bar->BarRect.Min.x + item.Offset + item.Width - ImGui::GetFontSize() * .5f -
								 bar->FramePadding.x,
							 (bar->BarRect.Min.y + bar->BarRect.Max.y) / 2};
				}
			Require(id, "First live tab is missing");
			SDL_Event motion{};
			motion.type = SDL_EVENT_MOUSE_MOTION;
			motion.motion.windowID = SDL_GetWindowID(test.window.Native());
			motion.motion.which = TestMouse;
			motion.motion.x = hover.x;
			motion.motion.y = hover.y;
			Require(SDL_PushEvent(&motion), "Cannot hover the live tab close button");
			frame();
			frame();
			const auto exportPath = directory / "closing-export.png";
			Require(tabs.ActiveRuntime().RequestExport(PathText(exportPath)) &&
						tabs.ActiveRuntime().ImageExport().Busy(),
					"Cannot start a live image export before tab closure");
			const auto beforeClose = tabs.Count();
			ImGui::ActivateItemByID(ImGui::GetIDWithSeed("#CLOSE", nullptr, id));
			frame();
			wait([&] { return tabs.Count() == beforeClose - 1; }, "Live tab did not close after its export");
			Require(IsCompletePng(exportPath), "Closing the live tab interrupted its export");
			for (std::size_t index = 1; index < apps.size(); ++index)
			{
				tabs.OpenApplication(apps[index]);
				wait(
					[&] {
						return tabs.ActiveRuntime().Endpoint() == endpoints[index] &&
							   tabs.ActiveRuntime().Snapshot().sessionRunning &&
							   tabs.ActiveRuntime().Snapshot().selectedReady;
					},
					"Closing one tab interrupted another live application");
			}
		}
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Vulkan validation errors in live tab workflow");
	std::cout << (std::getenv("SIMXR_TABS_BUDGET_QA") ? "PASS shared tab snapshot budget\n"
				  : std::getenv("SIMXR_TABS_PORTABLE_REPLAY_QA")
					  ? "PASS portable replay close\n"
					  : "PASS portable live tabs, input-independent pause, preview and close\n");
}

}
