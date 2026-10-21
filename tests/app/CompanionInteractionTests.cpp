#include "../TestSupport.h"
#include "app/DeviceControlPanel.h"
#include "app/ManualInputController.h"
#include "app/ProcessSupervisor.h"
#include "app/RuntimeConnection.h"
#include "app/RuntimePanels.h"
#include "app/SimulationEdits.h"
#include "app/UiRenderer.h"
#include "app/Window.h"
#include "app/WorkspacePanels.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "simulation/CommandScheduler.h"
#include <cmath>
#include <thread>

#include <SDL3/SDL.h>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <imgui.h>
#include <imgui_internal.h>
#include <iostream>

namespace
{
using namespace simxr::app;

class Interaction
{
  public:
	Window window{true};
	std::shared_ptr<ValidationLog> validation = std::make_shared<ValidationLog>();
	UiRenderer renderer{window, true, validation, true};
	ProcessSupervisor processes;
	WorkspacePanels workspace{processes, false, ""};
	RuntimePanels runtime{renderer, "", &window};
	ImVec2 buttonCenter;
	ImVec2 inputCenter;
	char input[32]{};
	unsigned clicks = 0;

	void Frame()
	{
		Eventually(
			[&]
			{
				Require(window.PollEvents(), "Unexpected window closure");
				return renderer.BeginFrame();
			},
			"UI frame admission stalled");
		workspace.BeginFrame(runtime);
		ImGui::SetNextWindowPos({20, 140}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({350, 420}, ImGuiCond_Always);
		workspace.Draw(runtime);
		ImGui::SetNextWindowPos({400, 170}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({200, 150}, ImGuiCond_Always);
		ImGui::Begin("Interaction probe");
		if (ImGui::Button("Click probe"))
			++clicks;
		const auto minimum = ImGui::GetItemRectMin();
		const auto maximum = ImGui::GetItemRectMax();
		buttonCenter = {(minimum.x + maximum.x) / 2, (minimum.y + maximum.y) / 2};
		ImGui::InputText("Text", input, sizeof(input));
		const auto inputMinimum = ImGui::GetItemRectMin();
		inputCenter = {inputMinimum.x + 10, inputMinimum.y + 10};
		ImGui::End();
		Require(renderer.Render(), "Admitted UI frame was not rendered");
	}
	void Click(ImVec2 position)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(window.Native());
		event.motion.x = position.x;
		event.motion.y = position.y;
		Require(SDL_PushEvent(&event), "Cannot enqueue pointer motion");
		Frame();
		event = {};
		event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
		event.button.windowID = SDL_GetWindowID(window.Native());
		event.button.button = SDL_BUTTON_LEFT;
		event.button.down = true;
		Require(SDL_PushEvent(&event), "Cannot enqueue mouse down");
		Frame();
		event.type = SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.down = false;
		Require(SDL_PushEvent(&event), "Cannot enqueue mouse up");
		Frame();
		Frame();
	}
	void SelectWorkspace(simxr::app::WorkspaceMode mode)
	{
		const auto* toolbar = ImGui::FindWindowByName("##Workspace toolbar");
		Require(toolbar != nullptr, "Workspace toolbar is missing");
		float x = toolbar->Pos.x + ImGui::GetStyle().WindowPadding.x;
		const char* labels[] = {"Simulate", "Inspect", "Record & replay"};
		for (int index = 0; index < static_cast<int>(mode); ++index)
			x += ImGui::CalcTextSize(labels[index]).x + ImGui::GetStyle().FramePadding.x * 2 +
				 ImGui::GetStyle().ItemSpacing.x;
		Click({x + 10, toolbar->Pos.y + ImGui::GetStyle().WindowPadding.y + ImGui::GetFrameHeight() / 2});
		Require(workspace.Mode() == mode, "Toolbar click did not switch workspace");
		const auto* rigs = ImGui::FindWindowByName("Rigs");
		Require((rigs && rigs->Active) == (mode == simxr::app::WorkspaceMode::Automation),
				"Rigs visibility does not follow the selected workspace");
	}
	void CheckFocusLoss()
	{
		window.TakeFocusLost();
		SDL_Event event{};
		event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
		event.window.windowID = SDL_GetWindowID(window.Native());
		Require(SDL_PushEvent(&event), "Cannot enqueue focus loss");
		Require(window.PollEvents() && window.TakeFocusLost(), "Focus loss was not delivered to the application");
		Require(!window.TakeFocusLost(), "Focus loss must be consumed once");
	}
	void CheckControls()
	{
		auto* rigs = ImGui::FindWindowByName("Rigs");
		Require(rigs && !rigs->Collapsed, "Rigs must initially be expanded");
		const ImVec2 collapse{rigs->Pos.x + ImGui::GetStyle().FramePadding.x + ImGui::GetFontSize() / 2,
							  rigs->Pos.y + rigs->TitleBarHeight / 2};
		Click(collapse);
		Require(rigs->Collapsed, "SDL click did not collapse Rigs");
		const auto before = clicks;
		Click(buttonCenter);
		Require(clicks == before + 1, "Other controls stopped responding with Rigs collapsed");
		Click(collapse);
		Require(!rigs->Collapsed, "SDL click did not reopen Rigs");
		Click(inputCenter);
		const auto length = std::char_traits<char>::length(input);
		SDL_Event text{};
		text.type = SDL_EVENT_TEXT_INPUT;
		text.text.windowID = SDL_GetWindowID(window.Native());
		text.text.text = "x";
		Require(SDL_PushEvent(&text), "Cannot enqueue text input");
		Frame();
		Frame();
		Require(std::char_traits<char>::length(input) == length + 1, "Text input stopped responding");
	}
};
void CheckDocking()
{
	Interaction test;
	const auto draw = [&]
	{
		Eventually(
			[&]
			{
				return test.window.PollEvents() && test.renderer.BeginFrame();
			},
			"Docking UI stalled");
		test.workspace.BeginFrame(test.runtime);
		test.workspace.DrawDockspace();
		test.workspace.Draw(test.runtime);
		test.runtime.Draw(test.workspace.Mode());
		Require(test.renderer.Render(), "Docking frame did not render");
	};
	for (int i = 0; i < 4; ++i)
		draw();
	const auto previewDock = ImGui::FindWindowByName("GPU preview")->DockId;
	const auto toolsDock = ImGui::FindWindowByName("Device controls")->DockId;
	Require(previewDock && toolsDock && previewDock != toolsDock, "Default tools should be beside the preview");
	test.workspace.SetMode(WorkspaceMode::Automation);
	for (int i = 0; i < 4; ++i)
		draw();
	Require(ImGui::FindWindowByName("Recording")->DockId == toolsDock, "Recording has no predictable dock");
	ImGui::DockBuilderDockWindow("Recording", previewDock);
	for (int i = 0; i < 3; ++i)
		draw();
	for (const auto mode : {WorkspaceMode::Simulator, WorkspaceMode::Debug, WorkspaceMode::Automation})
	{
		test.workspace.SetMode(mode);
		for (int i = 0; i < 4; ++i)
			draw();
	}
	Require(ImGui::FindWindowByName("Recording")->DockId == previewDock,
			"Tool switching undocked a user-positioned recording panel");
	test.workspace.SetMode(WorkspaceMode::Simulator);
	for (int i = 0; i < 3; ++i)
		draw();
	// Exercise the actual toolbar toggle, including keeping the old dockspace alive.
	const auto toggle = [&]
	{
		const auto toolbar = ImGui::FindWindowByName("##Workspace toolbar");
		float x = toolbar->Pos.x + ImGui::GetStyle().WindowPadding.x;
		for (const auto label : {"Simulator", "Inspect frames", "Record & replay"})
			x += ImGui::CalcTextSize(label).x + ImGui::GetStyle().FramePadding.x * 2 + ImGui::GetStyle().ItemSpacing.x;
		auto& io = ImGui::GetIO();
		io.AddMousePosEvent(x + 5, toolbar->Pos.y + ImGui::GetStyle().WindowPadding.y + 5);
		draw();
		io.AddMouseButtonEvent(0, true);
		draw();
		io.AddMouseButtonEvent(0, false);
		draw();
		for (int i = 0; i < 3; ++i)
			draw();
	};
	toggle();
	const auto expanded = ImGui::FindWindowByName("GPU preview");
	Require(expanded->DockId == 0 && expanded->Size.x >= ImGui::GetMainViewport()->WorkSize.x - 1,
			"Expand view did not fill the workspace");
	toggle();
	Require(ImGui::FindWindowByName("GPU preview")->DockId == previewDock, "Showing tools lost the preview dock");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Docking generated validation errors");
	std::cout << "PASS user docking survives tool switches; expand/restore preserves layout\n";
}
// Opt-in walkthrough against a separately launched application, never the user's session.
void PilotLiveRuntime(const char* endpoint)
{
	Interaction test;
	test.runtime.ConnectEndpoint(endpoint);
	test.workspace.SetMode(WorkspaceMode::Simulator);
	const auto frame = [&]
	{
		Eventually(
			[&]
			{
				return test.window.PollEvents() && test.renderer.BeginFrame();
			},
			"Pilot UI stalled");
		if (test.window.TakeFocusLost())
			test.runtime.ReleaseInputs();
		test.runtime.Poll();
		test.workspace.BeginFrame(test.runtime);
		test.workspace.DrawDockspace();
		test.workspace.Draw(test.runtime);
		test.runtime.Draw(test.workspace.Mode());
		Require(test.renderer.Render(test.runtime.ReadyWaits()), "Pilot UI frame was not rendered");
		test.runtime.Submitted(test.renderer.SubmittedFrames());
	};
	Eventually(
		[&]
		{
			frame();
			return test.runtime.Snapshot().selectedReady;
		},
		"Application did not supply a live preview");
	namespace wire = simxr::control;
	RuntimeConnection observer;
	observer.Connect(endpoint);
	Eventually(
		[&]
		{
			frame();
			return observer.Snapshot().state == RuntimeConnectionState::Connected;
		},
		"Observer connection failed");
	const auto observe = [&](simxr::ControlChannel channel = simxr::ControlChannel::Head)
	{
		const auto target = test.runtime.Snapshot();
		wire::ControlPacket request;
		request.message.opcode = static_cast<std::uint16_t>(wire::OpCode::SimulationStatus);
		request.message.fields = {wire::Field::U64(wire::tag::Session, target.session),
								  wire::Field::U64(wire::tag::Generation, target.generation)};
		const auto ticket = observer.Submit(std::move(request));
		Require(ticket != 0, "Cannot observe pilot input");
		std::optional<simxr::PoseSample> pose;
		Eventually(
			[&]
			{
				frame();
				for (const auto& reply : observer.Poll())
					if (reply.ticket == ticket)
						pose = ObservedDevicePose(reply.packet.message, channel);
				return pose.has_value();
			},
			"Pilot observation stalled");
		return *pose;
	};
	const auto before = observe();
	auto* preview = ImGui::FindWindowByName("GPU preview");
	ImVec2 center = preview->InnerRect.GetCenter();
	for (auto* child : preview->DC.ChildWindows)
		if (std::strstr(child->Name, "Image area"))
			center = child->InnerRect.GetCenter();
	SDL_Event event{};
	event.type = SDL_EVENT_MOUSE_MOTION;
	event.motion.windowID = SDL_GetWindowID(test.window.Native());
	event.motion.x = center.x;
	event.motion.y = center.y;
	SDL_PushEvent(&event);
	frame();
	event = {};
	event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
	event.button.windowID = SDL_GetWindowID(test.window.Native());
	event.button.button = SDL_BUTTON_RIGHT;
	event.button.down = true;
	SDL_PushEvent(&event);
	Eventually(
		[&]
		{
			frame();
			return SDL_GetWindowRelativeMouseMode(test.window.Native());
		},
		"Right mouse did not capture the live preview");
	event = {};
	event.type = SDL_EVENT_KEY_DOWN;
	event.key.windowID = SDL_GetWindowID(test.window.Native());
	event.key.scancode = SDL_SCANCODE_W;
	event.key.key = SDLK_W;
	event.key.down = true;
	SDL_PushEvent(&event);
	const auto pilotStarted = std::chrono::steady_clock::now();
	const auto appliedBefore = test.runtime.Snapshot().appliedPilotUpdates;
	for (unsigned index = 0; index < 30; ++index)
		frame();
	std::cout << "Piloting with rendering: " << test.runtime.Snapshot().appliedPilotUpdates - appliedBefore
			  << " applied updates over 30 UI frames in "
			  << std::chrono::duration<double>(std::chrono::steady_clock::now() - pilotStarted).count() << " seconds\n";
	event.type = SDL_EVENT_KEY_UP;
	event.key.down = false;
	SDL_PushEvent(&event);
	event = {};
	event.type = SDL_EVENT_MOUSE_BUTTON_UP;
	event.button.windowID = SDL_GetWindowID(test.window.Native());
	event.button.button = SDL_BUTTON_RIGHT;
	SDL_PushEvent(&event);
	for (unsigned index = 0; index < 20; ++index)
		frame();
	Require(!SDL_GetWindowRelativeMouseMode(test.window.Native()), "Mouse-up retained the pointer");
	const auto after = observe();
	const auto distance = [](simxr::PoseSample a, simxr::PoseSample b)
	{
		const auto x = a.pose.position.x - b.pose.position.x, y = a.pose.position.y - b.pose.position.y,
				   z = a.pose.position.z - b.pose.position.z;
		return std::sqrt(x * x + y * y + z * z);
	};
	Require(distance(before, after) > 0.02f, "WASD did not move the application's observed head pose");
	for (unsigned index = 0; index < 30; ++index)
		frame();
	Require(distance(after, observe()) < 0.001f, "Movement continued after releasing the controls");
	std::cout << "PASS live application SDL piloting: moved " << distance(before, after)
			  << " metres; mouse-up stops movement\n";
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		std::filesystem::create_directories(directory);
		test.renderer.RequestScreenshot((std::filesystem::path(directory) / "cemu-pilot.bmp").string());
		frame();
		frame();
	}
	auto* devices = ImGui::FindWindowByName("Device controls");
	auto* tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(devices->GetID("Devices"));
	const auto click = [&](ImVec2 point, Uint8 button, bool down)
	{
		SDL_Event pointer{};
		pointer.type = SDL_EVENT_MOUSE_MOTION;
		pointer.motion.windowID = SDL_GetWindowID(test.window.Native());
		pointer.motion.x = point.x;
		pointer.motion.y = point.y;
		SDL_PushEvent(&pointer);
		frame();
		pointer = {};
		pointer.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		pointer.button.windowID = SDL_GetWindowID(test.window.Native());
		pointer.button.button = button;
		pointer.button.down = down;
		SDL_PushEvent(&pointer);
		frame();
	};
	const ImVec2 handTab{tabs->BarRect.Min.x + tabs->Tabs[1].Offset + tabs->Tabs[1].Width / 2,
						 tabs->BarRect.GetCenter().y};
	click(handTab, SDL_BUTTON_LEFT, true);
	click(handTab, SDL_BUTTON_LEFT, false);
	const auto handBefore = observe(simxr::ControlChannel::LeftPose);
	click(center, SDL_BUTTON_RIGHT, true);
	Eventually(
		[&]
		{
			frame();
			return SDL_GetWindowRelativeMouseMode(test.window.Native());
		},
		"Hand pilot did not capture pointer");
	event = {};
	event.type = SDL_EVENT_MOUSE_MOTION;
	event.motion.windowID = SDL_GetWindowID(test.window.Native());
	event.motion.xrel = 40;
	event.motion.yrel = 20;
	SDL_PushEvent(&event);
	for (unsigned index = 0; index < 15; ++index)
		frame();
	const auto handAfter = observe(simxr::ControlChannel::LeftPose);
	Require(std::abs(handAfter.pose.orientation.y - handBefore.pose.orientation.y) > 0.02f,
			"Relative pointer input did not rotate the selected controller");
	Require(distance(after, observe()) < 0.001f, "Hand piloting also moved the head");
	event = {};
	event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
	event.window.windowID = SDL_GetWindowID(test.window.Native());
	SDL_PushEvent(&event);
	frame();
	Require(!SDL_GetWindowRelativeMouseMode(test.window.Native()), "Focus loss retained the pilot pointer");
	std::cout << "PASS live application selected-hand rotation; head unchanged; focus loss releases pointer\n";
	test.runtime.ReleaseInputs();
	for (unsigned index = 0; index < 5; ++index)
		frame();
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors during actual application piloting");
	// Keep the renderer idle: delivery must not depend on UI reply harvesting.
	for (unsigned index = 0; index < 8; ++index)
		frame();
	ManualInputController input;
	const auto target = test.runtime.Snapshot();
	input.SetTarget({endpoint, target.session, target.generation});
	Eventually(
		[&]
		{
			return input.Snapshot().observation != 0;
		},
		"Input worker did not observe the session");
	auto pose = ObservedDevicePose(input.Snapshot().state, simxr::ControlChannel::Head);
	const auto start = std::chrono::steady_clock::now();
	for (unsigned index = 0; index < 120; ++index)
	{
		pose.pose.position.x += 0.001f;
		ManualTransaction edit;
		edit.commands = {simxr::TakeControl{simxr::ControlChannel::Head, false}, simxr::SetHeadPose{pose}};
		Require(input.Submit(edit, true), "Continuous input was not accepted");
		std::this_thread::sleep_for(std::chrono::milliseconds(8));
	}
	Eventually(
		[&]
		{
			const auto state = input.Snapshot();
			Require(state.error.empty(), state.error.c_str());
			return !state.posePending &&
				   distance(pose, ObservedDevicePose(state.state, simxr::ControlChannel::Head)) < 0.001f;
		},
		"Latest pose did not arrive with the UI idle");
	const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - start).count();
	std::cout << "PASS input delivery with UI idle: " << input.Snapshot().appliedPoses << " applied poses in "
			  << seconds << " seconds; final pose matches\n";
	Require(input.Submit(ReleaseManualInputs{}), "Worker could not release manual controls");
	Eventually(
		[&]
		{
			return input.Snapshot().result.fields.empty();
		},
		"Manual release did not complete with the UI idle");
	// Exercise the inspection context on the same isolated real application.
	test.workspace.SetMode(WorkspaceMode::Debug);
	test.runtime.Command(simxr::control::CommandKind::Pause);
	Eventually(
		[&]
		{
			frame();
			return test.runtime.Snapshot().manual && !test.runtime.Snapshot().operationPending;
		},
		"Application did not pause");
	test.runtime.Command(simxr::control::CommandKind::Step);
	Eventually(
		[&]
		{
			frame();
			const auto state = test.runtime.Snapshot();
			return !state.operationPending && state.selectedCapture && state.selectedReady && !state.following;
		},
		"Step did not select its captured frame");
	const auto retained = test.runtime.Snapshot().selectedCapture;
	const auto captureContext = [&](const char* name)
	{
		if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot((std::filesystem::path(directory) / name).string());
			frame();
			frame();
		}
	};
	for (int i = 0; i < 6; ++i)
		frame();
	ImGui::SetWindowFocus("Historical state");
	for (int i = 0; i < 3; ++i)
		frame();
	captureContext("cemu-inspection-paused.bmp");
	test.runtime.ResumeLive();
	Eventually(
		[&]
		{
			frame();
			const auto state = test.runtime.Snapshot();
			return !state.operationPending && !state.manual && state.following;
		},
		"Continue did not resume and follow live output");
	test.runtime.SelectCapture(retained);
	for (int i = 0; i < 6; ++i)
		frame();
	Require(!test.runtime.Snapshot().manual && !test.runtime.Snapshot().following,
			"Inspecting a retained frame falsely paused the application");
	captureContext("cemu-inspection-running.bmp");
	test.runtime.SetLive(true);
	frame();
	Require(test.runtime.Snapshot().following && !test.runtime.Snapshot().manual,
			"Showing live output changed the application pause state");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors in frame inspection and thumbnail rendering");
	std::cout << "PASS real application inspection: pause, next frame, captured image, continue and historical view "
				 "while running\n";
}
void ConfigureObservedDevices()
{
	Interaction test;
	DeviceControlPanel panel;
	simxr::CommandConfiguration initial;
	initial.views.ipd = 0.073f;
	initial.views.outerWidth = 1760;
	initial.tracking.body.preset = simxr::BodyPreset::Seated;
	initial.tracking.gaze.yaw = 0.25f;
	simxr::CommandScheduler scheduler;
	scheduler.Start(0, initial);
	const auto observe = [&](std::uint64_t frame)
	{
		scheduler.Apply(simxr::FrameId{frame}, simxr::SimulationTime{simxr::Duration{100}},
						simxr::RuntimeTime{simxr::Duration{200}}, {});
		simxr::control::Message state;
		state.fields.push_back(simxr::control::Field::Bytes(
			simxr::control::tag::Document, simxr::control::SimulationState(scheduler.Snapshot()).Encode()));
		panel.ObserveState(state);
	};
	observe(1);
	std::optional<SimulationEdit> edit;
	const auto frame = [&]
	{
		Eventually(
			[&]
			{
				return test.window.PollEvents() && test.renderer.BeginFrame();
			},
			"Configuration UI stalled");
		ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({400, 650}, ImGuiCond_Always);
		ImGui::Begin("Configuration workflow");
		edit = panel.Draw(true);
		ImGui::End();
		Require(test.renderer.Render(), "Configuration frame did not render");
	};
	for (int i = 0; i < 3; ++i)
		frame();
	auto* window = ImGui::FindWindowByName("Configuration workflow");
	auto* tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(window->GetID("Devices"));
	Require(tabs && tabs->Tabs.Size == 4, "Configuration tab missing");
	ImGui::TabBarQueueFocus(tabs, &tabs->Tabs[3]);
	frame();
	frame();
	const auto details = [&]() -> ImGuiWindow*
	{
		for (auto* child : window->DC.ChildWindows)
			if (std::strstr(child->Name, "Configuration details"))
				return child;
		throw std::runtime_error("Setup detail panel is missing");
	};
	const auto activate = [&](const char* label)
	{
		ImGui::ActivateItemByID(details()->GetID(label));
		frame();
		Require(edit && std::holds_alternative<ManualTransaction>(*edit),
				(std::string("Configuration action did not issue an edit: ") + label).c_str());
		return std::get<ManualTransaction>(*edit);
	};
	auto transaction = activate("Apply view settings");
	const auto views = std::get<simxr::SetViewSettings>(transaction.commands.back()).value;
	Require(views.ipd == 0.073f && views.outerWidth == 1760,
			"Applying the view panel overwrote observed configuration with defaults");
	const auto selectSection = [&](int index)
	{
		ImGui::ActivateItemByID(ImHashStr("##Setup section", 0, tabs->Tabs[3].ID));
		frame();
		frame();
		auto* popup = ImGui::FindWindowByName("##Combo_00");
		Require(popup && popup->Active, "Setup categories are not browsable");
		const ImVec2 point{popup->WorkRect.Min.x + 30, popup->WorkRect.Min.y + ImGui::GetTextLineHeight() / 2 +
														   index * ImGui::GetTextLineHeightWithSpacing()};
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(test.window.Native());
		event.motion.x = point.x;
		event.motion.y = point.y;
		SDL_PushEvent(&event);
		frame();
		event = {};
		event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
		event.button.windowID = SDL_GetWindowID(test.window.Native());
		event.button.button = SDL_BUTTON_LEFT;
		event.button.down = true;
		SDL_PushEvent(&event);
		frame();
		event.type = SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.down = false;
		SDL_PushEvent(&event);
		frame();
		frame();
		if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
		{
			std::filesystem::create_directories(directory);
			test.renderer.RequestScreenshot(
				(std::filesystem::path(directory) / ("configuration-section-" + std::to_string(index) + ".bmp"))
					.string());
			frame();
			frame();
		}
	};
	selectSection(1);
	transaction = activate("Apply body pose");
	Require(std::get<simxr::SetBodyControls>(transaction.commands.back()).value.preset == simxr::BodyPreset::Seated,
			"Body UI lost the observed preset");
	selectSection(2);
	transaction = activate("Apply gaze");
	Require(std::get<simxr::SetGaze>(transaction.commands.back()).value.yaw == 0.25f,
			"Gaze UI started from a default instead of observed values");
	selectSection(3);
	transaction = activate("Apply tracker");
	Require(std::get<simxr::SetTracker>(transaction.commands.back()).value.identity.value ==
				initial.tracking.trackers[0].identity.value,
			"Tracker UI changed device identity");
	selectSection(4);
	transaction = activate("Activate scenario");
	Require(std::get<simxr::TakeControl>(transaction.commands.front()).channel == simxr::ControlChannel::Head,
			"Scenario action did not claim its affected channel");
	simxr::CommandTransaction command;
	command.id = 1;
	command.source = {simxr::CommandSourceKind::Manual, 77};
	command.target.frame = simxr::FrameId{2};
	command.commands = transaction.commands;
	scheduler.Submit(command);
	observe(2);
	frame();
	transaction = activate("Restore normal behavior");
	Require(!std::get<simxr::SetScenario>(transaction.commands.back()).enabled,
			"Scenario recovery did not clear the failure");
	command.id = 2;
	command.target.frame = simxr::FrameId{3};
	command.commands = transaction.commands;
	scheduler.Submit(command);
	observe(3);
	frame();
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		std::filesystem::create_directories(directory);
		test.renderer.RequestScreenshot((std::filesystem::path(directory) / "configuration-recovery.bmp").string());
		frame();
		frame();
	}
	panel.ResetObservations();
	frame();
	ImGui::ActivateItemByID(details()->GetID("Activate scenario"));
	frame();
	Require(!edit, "A disconnected panel applied values from its previous session");
	initial.views.ipd = 0.061f;
	scheduler.Start(0, initial);
	observe(1);
	frame();
	selectSection(0);
	transaction = activate("Apply view settings");
	Require(std::get<simxr::SetViewSettings>(transaction.commands.back()).value.ipd == 0.061f,
			"Reconnection retained stale configuration");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Configuration UI produced Vulkan validation errors");
	std::cout << "PASS production settings panels: observed values, categories, scenario recovery and reconnect\n";
}
void CaptureLayouts(const std::filesystem::path& directory)
{
	std::filesystem::create_directories(directory);
	Interaction test;
	for (const auto size : {ImVec2{1280, 720}, ImVec2{1920, 1080}})
		for (const float scale : {1.0f, 1.5f})
		{
			test.window.Resize(static_cast<int>(size.x), static_cast<int>(size.y));
			test.workspace.SetMode(WorkspaceMode::Simulator);
			test.workspace.SetScale(scale);
			const auto path = directory / (std::to_string(static_cast<int>(size.x)) + "-" +
										   std::to_string(static_cast<int>(scale * 100)) + ".bmp");
			ImVec2 handTab{};
			for (unsigned frame = 0; frame < 36; ++frame)
			{
				if (frame >= 12 && frame <= 14)
				{
					SDL_Event event{};
					if (frame == 12)
					{
						event.type = SDL_EVENT_MOUSE_MOTION;
						event.motion.windowID = SDL_GetWindowID(test.window.Native());
						event.motion.x = handTab.x;
						event.motion.y = handTab.y;
					}
					else
					{
						event.type = frame == 13 ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
						event.button.windowID = SDL_GetWindowID(test.window.Native());
						event.button.button = SDL_BUTTON_LEFT;
						event.button.down = frame == 13;
					}
					Require(SDL_PushEvent(&event), "Cannot select hand tab");
				}
				Eventually(
					[&]
					{
						return test.window.PollEvents() && test.renderer.BeginFrame();
					},
					"Layout frame stalled");
				test.runtime.Poll();
				test.workspace.BeginFrame(test.runtime);
				test.workspace.DrawDockspace();
				test.workspace.Draw(test.runtime);
				test.runtime.Draw(test.workspace.Mode());
				auto* devices = ImGui::FindWindowByName("Device controls");
				Require(devices != nullptr, "Device controls are missing");
				auto* tabs = ImGui::GetCurrentContext()->TabBars.GetByKey(devices->GetID("Devices"));
				Require(tabs && tabs->Tabs.Size >= 3, "Device tabs are missing");
				if (frame == 0)
					ImGui::TabBarQueueFocus(tabs, &tabs->Tabs[0]);
				if (frame == 11)
				{
					Require(tabs->SelectedTabId == tabs->Tabs[0].ID, "Head layout capture did not select the head tab");
					handTab = {tabs->BarRect.Min.x + tabs->Tabs[1].Offset + tabs->Tabs[1].Width / 2,
							   tabs->BarRect.GetCenter().y};
					test.renderer.RequestScreenshot(path.string());
				}
				if (frame == 23)
				{
					Require(tabs->SelectedTabId == tabs->Tabs[1].ID, "Disconnected device tabs must remain browsable");
					test.renderer.RequestScreenshot(
						(path.parent_path() / (path.stem().string() + "-hand.bmp")).string());
					Require(tabs->Tabs.Size == 4, "Setup tab is missing");
					ImGui::TabBarQueueFocus(tabs, &tabs->Tabs[3]);
				}
				if (frame == 35)
				{
					Require(tabs->SelectedTabId == tabs->Tabs[3].ID, "Setup is not reachable while disconnected");
					test.renderer.RequestScreenshot(
						(path.parent_path() / (path.stem().string() + "-setup.bmp")).string());
				}
				Require(test.renderer.Render(), "Layout frame was not rendered");
			}
			test.CheckFocusLoss();
			test.renderer.WaitIdle();
		}
	Require(test.validation->errors.load() == 0, "Validation errors during layout capture");
}
} // namespace

int main()
{
	try
	{
		auto validation = std::make_shared<ValidationLog>();
		{
			Interaction test;
			validation = test.validation;
			for (unsigned frame = 0; frame < 4; ++frame)
				test.Frame();
			test.SelectWorkspace(simxr::app::WorkspaceMode::Automation);
			test.CheckControls();
			test.SelectWorkspace(simxr::app::WorkspaceMode::Simulator);
			test.SelectWorkspace(simxr::app::WorkspaceMode::Debug);
			test.SelectWorkspace(simxr::app::WorkspaceMode::Automation);
			for (const auto size : {ImVec2{800, 600}, ImVec2{1024, 700}, ImVec2{900, 640}})
			{
				const auto rebuilds = test.renderer.RebuildCount();
				test.window.Resize(static_cast<int>(size.x), static_cast<int>(size.y));
				Eventually(
					[&]
					{
						test.Frame();
						return test.renderer.RebuildCount() > rebuilds;
					},
					"Resize did not rebuild the swapchain");
				test.CheckControls();
			}
			test.CheckFocusLoss();
			test.renderer.WaitIdle();
		}
		Require(validation->errors.load() == 0, "Vulkan validation errors during interaction or teardown");
		CheckDocking();
		ConfigureObservedDevices();
		if (const auto endpoint = std::getenv("SIMXR_UI_PILOT_ENDPOINT"))
			PilotLiveRuntime(endpoint);
		if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
			CaptureLayouts(directory);
		std::cout << "PASS workspace switching and SDL clicks collapse/reopen production Rigs across three resizes\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
