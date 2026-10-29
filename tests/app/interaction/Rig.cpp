#include "app/UiPch.h"
#include "Cases.h"
#include "simulation/TrackingSystem.h"
#include "simulation/CommandScheduler.h"
#include "control/FrameInspection.h"
#include "app/widgets/Theme.h"
#include "app/services/PlayerRig.h"
#include "app/services/ManualInputController.h"
#include "app/services/SimulationEdits.h"
#include "app/services/SpatialMath.h"
#include "app/services/RuntimeSession.h"
#include "app/widgets/PilotController.h"
#include "app/widgets/SpatialEditor.h"
#include "app/widgets/DocumentTree.h"
#include "app/widgets/DeviceRail.h"
#include "app/pages/DeviceControlPanel.h"
#include "app/services/InputHistory.h"
#include "app/services/Utf8Path.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include <iostream>
#include <random>

namespace simxr::tests
{
void PilotLiveRuntime(const char* endpoint)
{
	IgnoreRealPointer();
	Interaction test;
	Require(SDL_ShowWindow(test.window.Native()), "Show live application pilot window");
	SDL_RaiseWindow(test.window.Native());
	test.runtime.ConnectEndpoint(endpoint);
	test.workspace.Select(DeviceSel{});
	const auto frame = [&]
	{
		Eventually(
			[&]
			{
				return test.PollEvents() && test.renderer.BeginFrame();
			},
			"Pilot UI stalled");
		if (test.window.TakeFocusLost())
			test.runtime.FocusLost();
		test.runtime.Poll();
		test.workspace.BeginFrame(test.runtime);
		test.workspace.Draw(test.runtime);
		const auto transfers = [&](VkCommandBuffer command)
		{
			test.runtime.RecordTransfers(command);
		};
		Require(test.renderer.Render(test.runtime.ReadyWaits(), transfers), "Pilot UI frame was not rendered");
		test.runtime.Submitted(test.renderer.SubmittedFrames());
	};
	Eventually(
		[&]
		{
			frame();
			return test.runtime.Snapshot().selectedReady && test.window.Focused();
		},
		"Application did not supply a focused live preview");
	const auto activate = [&](const char* window, const char* label)
	{
		auto* target = FindPanel(window);
		Require(target != nullptr, "Control window is missing");
		ImGui::SetWindowFocus(target->Name);
		ImGui::ActivateItemByID(target->GetID(label));
		frame();
	};
	Require(std::getenv("SIMXR_COMPANION_SETTINGS"), "Isolated recording checks need a dedicated settings path");
	test.workspace.ShowLibrary(true);
	for (int i = 0; i < 4; ++i)
		frame();
	Eventually(
		[&]
		{
			frame();
			return !test.runtime.Snapshot().recordingBusy;
		},
		"Initial recording read did not finish");
	activate("Timeline", "Rec");
	frame();
	const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
	if (!popups.empty() && popups.back().Window &&
		popups.back().PopupId == FindPanel("##Workspace toolbar")->GetID("Replace unsaved take?"))
	{
		ImGui::ActivateItemByID(popups.back().Window->GetID("Record new take"));
		frame();
	}
	Eventually(
		[&]
		{
			frame();
			return test.runtime.Snapshot().recording;
		},
		"Recording did not start through the panel");
	test.workspace.Select(DeviceSel{});
	for (int i = 0; i < 4; ++i)
		frame();

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
	auto* preview = FindPanel("GPU preview");
	ImVec2 center = preview->InnerRect.GetCenter();
	for (auto* child : preview->DC.ChildWindows)
		if (std::strstr(child->Name, "Image area"))
			center = child->InnerRect.GetCenter();
	SDL_Event event{};
	event.type = SDL_EVENT_MOUSE_MOTION;
	event.motion.windowID = SDL_GetWindowID(test.window.Native());
	event.motion.which = TestMouse;
	event.motion.x = center.x;
	event.motion.y = center.y;
	SDL_PushEvent(&event);
	frame();
	event = {};
	event.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
	event.button.windowID = SDL_GetWindowID(test.window.Native());
	event.button.which = TestMouse;
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
	event.button.which = TestMouse;
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
		test.renderer.RequestScreenshot(std::filesystem::path(directory) / "cemu-pilot.bmp");
		frame();
		frame();
	}
	const auto click = [&](ImVec2 point, Uint8 button, bool down)
	{
		SDL_Event pointer{};
		pointer.type = SDL_EVENT_MOUSE_MOTION;
		pointer.motion.windowID = SDL_GetWindowID(test.window.Native());
		pointer.motion.which = TestMouse;
		pointer.motion.x = point.x;
		pointer.motion.y = point.y;
		SDL_PushEvent(&pointer);
		frame();
		pointer = {};
		pointer.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		pointer.button.windowID = SDL_GetWindowID(test.window.Native());
		pointer.button.which = TestMouse;
		pointer.button.button = button;
		pointer.button.down = down;
		SDL_PushEvent(&pointer);
		frame();
	};
	ImGui::ActivateItemByID(FindPanel("Device tabs")->GetID("Left"));
	frame();
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
	event.motion.which = TestMouse;
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
	Eventually(
		[&]
		{
			frame();
			return !test.runtime.Snapshot().recordingBusy;
		},
		"Recording refresh did not finish");
	const auto captureContext = [&](const char* name)
	{
		if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) / name);
			frame();
			frame();
			test.renderer.WaitIdle();
		}
	};
	// The earlier focus-loss injection must be paired before editing the take name.
	SDL_RaiseWindow(test.window.Native());
	event = {};
	event.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
	event.window.windowID = SDL_GetWindowID(test.window.Native());
	Require(SDL_PushEvent(&event), "Restore pilot UI focus");
	Eventually(
		[&]
		{
			frame();
			return test.window.Focused() && !ImGui::GetIO().AppFocusLost;
		},
		"Pilot UI did not regain focus");
	const auto savePopover = FindPanel("Top bar")->GetID("Recorded take");
	std::string savedPath;
	try
	{
		activate("Timeline", "Stop recording");
		Eventually(
			[&]
			{
				frame();
				const auto state = test.runtime.Snapshot();
				return !state.recording && !state.recordingBusy &&
					   ImGui::IsPopupOpen(savePopover, ImGuiPopupFlags_AnyPopupLevel);
			},
			"Stop recording did not finish and open the save popover");
		const auto stopped = test.runtime.Snapshot();
		Require(stopped.recordedChanges > 0 && !stopped.recordingOverflow,
				"Piloting did not produce a complete recording");
		frame();
		auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
		Require(popup, "Save popover has no window");
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		for (;;)
		{
			frame();
			if (const auto take = std::get_if<TakeSel>(&test.workspace.State().selection))
				savedPath = take->id;
			if (!savedPath.empty() && std::filesystem::exists(Utf8Path(savedPath) / "input.simxr") &&
				!test.runtime.Snapshot().recordingBusy)
				break;
			Require(std::chrono::steady_clock::now() < deadline, "Stop did not produce a persistent archive");
		}
		ImGui::ActivateItemByID(popup->GetID("Done"));
		frame();
		Require(!OpenSavedRecording(savedPath).path.empty(), "Saved take cannot be reopened independently");
	}
	catch (...)
	{
		const auto state = test.runtime.Snapshot();
		std::cout << "Pilot save: recording=" << state.recording << " busy=" << state.recordingBusy
				  << " overflow=" << state.recordingOverflow << " changes=" << state.recordedChanges
				  << " focused=" << test.window.Focused() << " appFocusLost=" << ImGui::GetIO().AppFocusLost
				  << " popup=" << ImGui::IsPopupOpen(savePopover, ImGuiPopupFlags_AnyPopupLevel)
				  << " path=" << savedPath << " error=" << state.error << '\n';
		for (const auto& notice : test.runtime.TakeNotices())
			std::cout << "Pilot notice: " << notice.title << " " << notice.detail << '\n';
		captureContext("cemu-save-failed.bmp");
		throw;
	}
	captureContext("cemu-recording-saved.bmp");
	Eventually(
		[&]
		{
			auto* inspector = FindPanel("Inspector");
			Require(inspector, "Saved take inspector is missing");
			const auto play = inspector->GetID("Play take");
			ImGui::SetFocusID(play, inspector);
			frame();
			const auto& context = *ImGui::GetCurrentContext();
			return context.NavId == play && context.NavIdIsAlive && test.runtime.CanReplay();
		},
		"Saved take did not become ready in the Library");
	activate("Inspector", "Play take");
	Eventually(
		[&]
		{
			frame();
			return test.workspace.State().timeline.playing && test.runtime.Snapshot().manual;
		},
		"Saved take did not enter replay");
	captureContext("cemu-recording-replay.bmp");
	activate("Inspector", "Stop take");
	Eventually(
		[&]
		{
			frame();
			const auto state = test.runtime.Snapshot();
			return !test.workspace.State().timeline.playing && !state.manual && !state.operationPending;
		},
		"Cancelling replay did not release frame control");
	const auto observation = input.Snapshot().observation;
	input.Refresh();
	Eventually(
		[&]
		{
			frame();
			return input.Snapshot().observation > observation;
		},
		"Stopped replay input observation did not arrive");
	const auto released = ReadObservedSetup(input.Snapshot().state);
	Require(std::none_of(released.owners.begin(), released.owners.end(),
						 [](const auto& owner)
						 {
							 return owner && owner->kind == simxr::CommandSourceKind::Rig;
						 }),
			"Cancelled replay retained input ownership");
	std::cout << "PASS Cemu record while piloting, global stop, save, independent reopen, replay and cancel release\n";

	SDL_SetEventFilter(nullptr, nullptr);
	test.live = true;
	test.runtime.SetLive(true);
	CheckTimelineWorkflow(test);
	test.workspace.Select(DeviceSel{DevicePage::Head});
	frame();
	frame();
	const auto timelineHeight = FindPanel("Timeline")->Size.y;
	activate("Timeline divider tab", "##Edge");
	frame();
	frame();
	Require(FindPanel("Timeline")->Size.y < timelineHeight * .6f, "Live application timeline did not fold");
	captureContext("cemu-timeline-folded.bmp");
	activate("Timeline divider tab", "##Edge");
	frame();
	frame();
	Require(std::abs(FindPanel("Timeline")->Size.y - timelineHeight) < 1,
		"Live application timeline did not restore its height");
	const auto previewWidth = FindPanel("GPU preview")->Size.x;
	activate("Inspector divider tab", "##Edge");
	frame();
	frame();
	Require(!test.workspace.State().inspectorVisible && FindPanel("GPU preview")->Size.x > previewWidth + 100,
		"Live application inspector did not fold");
	captureContext("cemu-inspector-folded.bmp");
	activate("Inspector divider tab", "##Edge");
	frame();
	frame();
	Require(test.workspace.State().inspectorVisible &&
		std::abs(FindPanel("GPU preview")->Size.x - previewWidth) < 1,
		"Live application inspector did not restore its width");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors during real application shell folding");
	std::cout << "PASS live application timeline and inspector fold and restore\n";
}
void CheckDocumentTreeIdentity()
{
	using simxr::control::InspectionValue;
	Interaction test;
	RuntimeSession session{test.renderer, ""};
	DocumentTree tree{session};
	const auto document = InspectionValue::Object({
		{"Earlier", InspectionValue::Object({{"One", InspectionValue::Unsigned(1)}})},
		{"Later", InspectionValue::Object({{"Two", InspectionValue::Unsigned(2)},
			{"Three", InspectionValue::Unsigned(3)}})}}).Encode("Root");
	ImVec2 origin;
	float height = 0, row = 0;
	const auto frame = [&]
	{
		Eventually([&] { return test.PollEvents() && test.renderer.BeginFrame(); }, "Document tree fixture stalled");
		ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({440, 650}, ImGuiCond_Always);
		ImGui::Begin("Document identity");
		origin = ImGui::GetCursorScreenPos();
		row = ImGui::GetTextLineHeightWithSpacing();
		tree.Begin(1);
		ImGui::SetNextItemOpen(true, ImGuiCond_Always);
		tree.Draw("Captured state", document);
		height = ImGui::GetCursorScreenPos().y - origin.y;
		ImGui::End();
		Require(test.renderer.Render(), "Document tree fixture did not render");
	};
	for (int count = 0; count < 3; ++count)
		frame();
	auto* window = FindPanel("Document identity");
	Require(window, "Document tree fixture window missing");
	ImGui::SetWindowFocus(window->Name);
	const auto clickRow = [&](int index)
	{
		auto& io = ImGui::GetIO();
		io.AddMousePosEvent(origin.x + ImGui::GetStyle().IndentSpacing + 5, origin.y + row * (index + .5f));
		frame();
		io.AddMouseButtonEvent(0, true);
		frame();
		io.AddMouseButtonEvent(0, false);
		frame();
		frame();
	};
	const float collapsed = height;
	clickRow(2);
	const float laterOpen = height;
	Require(std::abs(laterOpen - collapsed - 2 * row) < 1, "Later inspection branch did not open");
	clickRow(1);
	Require(std::abs(height - laterOpen - row) < 1,
		"Expanding an earlier inspection branch changed the later branch's fold state");
	clickRow(1);
	Require(std::abs(height - laterOpen) < 1,
		"Collapsing an earlier inspection branch changed the later branch's fold state");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Document tree validation errors");
	std::cout << "PASS inspection tree sibling identity across expansion and collapse\n";
}

void CheckRigPointer()
{
	Interaction test;
	IgnoreRealPointer();
	RigViewState state;
	auto scene = SpatialScene::FromTracking(simxr::TrackingSystem::Evaluate(
		simxr::RuntimeTime{simxr::Duration{200}}, simxr::TrackingState{}, simxr::BodyModel{}));
	int selected = -1;
	const auto frame = [&]
	{
		Eventually(
			[&]
			{
				return test.PollEvents() && test.renderer.BeginFrame();
			},
			"Rig pointer fixture stalled");
		ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({500, 500}, ImGuiCond_Always);
		ImGui::Begin("Rig pointer");
		RigView("Rig", state, scene, selected, {}, {440, 400});
		ImGui::End();
		Require(test.renderer.Render(), "Rig pointer fixture did not render");
	};
	frame();
	frame();
	ImGui::SetWindowFocus("Rig pointer");
	const auto move = [&](float x, float y)
	{
		SDL_WarpMouseInWindow(test.window.Native(), x, y);
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(test.window.Native());
		event.motion.which = TestMouse;
		event.motion.x = x;
		event.motion.y = y;
		Require(SDL_PushEvent(&event), "Push absolute rig motion");
	};
	const auto press = [&](int button, bool down)
	{
		SDL_Event event{};
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.windowID = SDL_GetWindowID(test.window.Native());
		event.button.which = TestMouse;
		event.button.button = button == ImGuiMouseButton_Left ? SDL_BUTTON_LEFT : SDL_BUTTON_MIDDLE;
		event.button.down = down;
		Require(SDL_PushEvent(&event), "Push rig button");
	};
	for (const auto button : {ImGuiMouseButton_Left, ImGuiMouseButton_Middle})
	{
		move(state.minimum.x + 100, state.minimum.y + 320);
		frame();
		const auto yaw = state.yaw;
		const auto pan = state.pan;
		const ImVec2 start{state.minimum.x + 350, state.minimum.y + 320};
		move(start.x, start.y);
		press(button, true);
		frame();
		Require(state.yaw == yaw && state.pan.x == pan.x && state.pan.y == pan.y && state.pan.z == pan.z,
				"Rig jumped from movement before the mouse press");
		move(start.x + 20, start.y + 10);
		frame();
		Require(button == ImGuiMouseButton_Left ? state.yaw != yaw : state.pan.x != pan.x,
				"Absolute pointer drag did not move the rig camera");
		press(button, false);
		frame();
		const auto stoppedYaw = state.yaw;
		const auto stoppedPan = state.pan;
		move(start.x - 20, start.y - 10);
		frame();
		Require(state.yaw == stoppedYaw && state.pan.x == stoppedPan.x, "Rig camera moved after release");
	}
	const auto head = state.points[0];
	move(head.x, head.y);
	press(ImGuiMouseButton_Left, true);
	frame();
	Require(selected == 0 && !state.orbiting, "Absolute pointer click did not select the head");
	press(ImGuiMouseButton_Left, false);
	frame();
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Rig pointer validation errors");
	SDL_SetEventFilter(nullptr, nullptr);
	std::cout << "PASS absolute rig clicks, orbit and pan without press jumps\n";
}

void CheckCapturedRig()
{
	Interaction test;
	RuntimeSession session{test.renderer, ""};
	DocumentTree documents{session};
	FramePage page{session, documents};
	RuntimeView view;
	view.epoch = view.session = view.generation = 1;
	view.time = {true, {1, 1, 1}, 1};
	ShellState shell;
	shell.selection = FrameSel{{1, 1, 1}, 1};
	auto& workspace = session.Workspace();
	workspace.SetScope({1, 1, 1});
	Require(workspace.Add(1, 1) && workspace.Select(1), "Recorded rig fixture could not select its capture");
	FrameRecord record;
	record.key = {{1}, {1}};
	record.stage = FrameStage::Submitted;
	record.submission.emplace();
	record.submission->tracking = TrackingSystem::Evaluate(RuntimeTime{Duration{200}}, TrackingState{}, BodyModel{});
	control::InspectionStore store;
	control::Message document;
	document.fields = {control::Field::U64(control::tag::InspectionId, 1),
		control::Field::Bytes(control::tag::Document, control::InspectFrame(store, {1}, record)->document)};
	workspace.Handle({PanelWork::Inspect, 1}, document);
	ImVec2 expandedSize;
	const auto frame = [&]
	{
		Eventually([&] { return test.PollEvents() && test.renderer.BeginFrame(); }, "Captured rig fixture stalled");
		page.Prepare(shell.selection, view);
		ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({440, 650}, ImGuiCond_Always);
		ImGui::Begin("Captured rig");
		if (page.Expanded())
			page.DrawExpanded(expandedSize);
		else
			page.Draw(shell, view);
		ImGui::End();
		Require(test.renderer.Render(), "Captured rig fixture did not render");
	};
	const auto localRight = [&]
	{
		const auto point = page.RigDevicePoints()[2];
		const auto area = page.RigViewRect();
		return ImVec2{point.x - area.x, point.y - area.y};
	};
	for (int count = 0; count < 3; ++count)
		frame();
	const auto initialArea = page.RigViewRect();
	const auto captured = localRight();
	Require(initialArea.width > 0 && captured.x > 0 && captured.y > 0,
		"Captured rig did not display recorded hand geometry while following live output");
	view.time.cursor.frame = 2;
	frame();
	Require(std::hypot(localRight().x - captured.x, localRight().y - captured.y) < .01f,
		"The live cursor changed the selected frame's recorded pose");
	expandedSize = {initialArea.width, initialArea.height};
	page.SetExpanded(true);
	frame();
	Require(std::hypot(localRight().x - captured.x, localRight().y - captured.y) < .01f,
		"Expanding a captured rig changed its recorded pose source");
	Require(std::holds_alternative<FrameSel>(shell.selection) && workspace.Selected()->document.Find(control::tag::Document)->value ==
		document.Find(control::tag::Document)->value, "Viewing a recorded rig changed its capture");
	shell.selection = FrameSel{{1, 1, 2}, 1};
	frame();
	Require(page.Active() && page.RigViewRect().width == 0 && page.RigDevicePoints()[2].x == 0,
		"Missing selected-frame metadata reused another capture's rig geometry");
	auto* window = FindPanel("Captured rig");
	Require(window, "Captured rig fixture window missing");
	ImGui::ActivateItemByID(window->GetID("Back to image##Recorded rig unavailable"));
	frame();
	Require(!page.Expanded(), "Unavailable recorded rig could not return to the image");
	shell.selection = DeviceSel{DevicePage::Head};
	page.Prepare(shell.selection, view);
	Require(!page.Active() && !page.Expanded(), "Leaving Frame retained the recorded view context");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Captured rig validation errors");
	std::cout << "PASS captured rig identity, read-only page and expanded view, and missing metadata retirement\n";
}

void CheckSpatialWidgetLifecycle()
{
	Interaction test;
	PlayerRig rig;
	ManualInputController input;
	UndoStack undo;
	SpatialEditor editor{rig, input, undo};
	CommandScheduler scheduler;
	scheduler.Start();
	scheduler.Apply(FrameId{1}, SimulationTime{Duration{100}}, RuntimeTime{Duration{200}}, {});
	ManualInputSnapshot snapshot;
	snapshot.observation = 1;
	snapshot.state.fields.push_back(control::Field::Bytes(control::tag::Document,
		control::SimulationState(scheduler.Snapshot()).Encode()));
	editor.Observe(snapshot, false);
	Selection selection = DeviceSel{DevicePage::Right};
	bool placement = true, historical = false;
	std::uint64_t historyIdentity = 1;
	std::vector<FrameView> eyes{{rig.Head(), {-.8f, .8f, .8f, -.8f}}};
	FrameRecord record;
	record.key = {{1}, {1}};
	record.stage = FrameStage::Submitted;
	record.submission.emplace();
	record.submission->tracking = TrackingSystem::Evaluate(RuntimeTime{Duration{200}},
		ReadObservedSetup(snapshot.state).values.tracking, BodyModel{});
	control::InspectionStore store;
	control::Message document;
	document.fields.push_back(control::Field::Bytes(control::tag::Document,
		control::InspectFrame(store, {1}, record)->document));
	const auto frame = [&]
	{
		Eventually([&] { return test.PollEvents() && test.renderer.BeginFrame(); }, "Spatial fixture stalled");
		editor.SetContext(!historical, historical, eyes, historical ? &document : nullptr, historyIdentity);
		ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({440, 650}, ImGuiCond_Always);
		ImGui::Begin("Spatial lifecycle");
		if (historical)
			editor.DrawRig(selection, {400, 400});
		else if (placement)
			editor.DrawPlacement(selection);
		ImGui::End();
		editor.EndFrame();
		Require(test.renderer.Render(), "Spatial fixture did not render");
	};
	const auto pump = [&](int count) { for (int index = 0; index < count; ++index) frame(); };
	pump(3);
	auto* window = FindPanel("Spatial lifecycle");
	Require(window, "Spatial fixture window missing");
	ImGui::SetWindowFocus(window->Name);
	const auto orb = ImHashStr("Rotation ball", 0, window->GetID(2));
	const auto activateYaw = [&]
	{
		ImGui::ActivateItemByID(ImHashStr("##Yaw", 0, orb));
		ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
		frame();
		Require(editor.Active(), "Numeric rotation did not retain gesture ownership");
	};
	activateYaw();
	auto& io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiKey_A, true);
	frame();
	io.AddKeyEvent(ImGuiKey_A, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	frame();
	io.AddInputCharactersUTF8("25");
	frame();
	io.AddKeyEvent(ImGuiKey_Enter, true);
	frame();
	io.AddKeyEvent(ImGuiKey_Enter, false);
	frame();
	Require(std::abs(rig.Hand(1).orientation.y) > .1f, "Rejected-edit fixture never edited the rig");
	Require(undo.Empty(), "Rejected spatial submission entered undo history");
	activateYaw();
	placement = false;
	pump(2);
	Require(!editor.Active(), "Hidden numeric rotation retained gesture ownership");
	historical = true;
	selection = DeviceSel{DevicePage::Head};
	pump(2);
	const auto back = editor.RigDevicePoints()[2];
	ChooseRigView(window->GetID("Rig view"), "Eye", pump);
	const auto eye = editor.RigDevicePoints()[2];
	Require(std::hypot(eye.x - back.x, eye.y - back.y) > 10, "Historical Eye fixture did not change camera");
	eyes.clear();
	++historyIdentity;
	pump(2);
	const auto missing = editor.RigDevicePoints()[2];
	Require(std::hypot(missing.x - back.x, missing.y - back.y) < .01f,
		"Historical frame without eye metadata reused another frame's camera");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Spatial lifecycle validation errors");
	std::cout << "PASS numeric gesture ownership, hidden widget retirement, rejected undo and historical camera isolation\n";
}

void CheckExpandedTrackerUi()
{
	Interaction test;
	PlayerRig rig;
	DeviceControlPanel panel{rig};
	Selection selection = DeviceSel{DevicePage::Trackers, 63};
	for (const auto count : {64u, 0u, 5u})
	{
		CommandConfiguration config;
		config.tracking.trackers.clear();
		for (unsigned index = 0; index < count; ++index)
		{
			TrackerDevice device{{100 + index}, TrackerRole::Unassigned};
			device.poseOverride = XrPosef{{0, 0, 0, 1}, {float(index % 8) * .15f, 1, float(index / 8) * .15f}};
			config.tracking.trackers.push_back(device);
		}
		CommandScheduler scheduler;
		scheduler.Start(0, config);
		scheduler.Apply({1}, {Duration{100}}, {Duration{200}}, {});
		control::Message state;
		state.fields.push_back(control::Field::Bytes(control::tag::Document,
			control::SimulationState(scheduler.Snapshot()).Encode()));
		panel.ObserveState(state);
		auto scene = SpatialScene::FromTracking(TrackingSystem::Evaluate({Duration{200}}, config.tracking, BodyModel{}));
		Require(std::count(scene.available.begin() + 3, scene.available.end(), true) == count,
			"Spatial scene omitted available trackers");
		RigViewState view;
		int selected = count ? static_cast<int>(count + 2) : -1;
		for (unsigned frame = 0; frame < 3; ++frame)
		{
			Eventually([&] { return test.PollEvents() && test.renderer.BeginFrame(); }, "Tracker UI stalled");
			ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({420, 650}, ImGuiCond_Always);
			ImGui::Begin("Tracker inventory");
			panel.DrawHeader(selection, true);
			panel.Draw(true, &selection);
			ImGui::End();
			ImGui::SetNextWindowPos({460, 20}, ImGuiCond_Always);
			ImGui::SetNextWindowSize({560, 650}, ImGuiCond_Always);
			ImGui::Begin("Expanded rig");
			RigView("Trackers", view, scene, selected, {}, {520, 560});
			ImGui::End();
			if (frame == 2)
				if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
					test.renderer.RequestScreenshot(std::filesystem::path(directory) / ("trackers-" + std::to_string(count) + ".bmp"));
			Require(test.renderer.Render(), "Expanded tracker UI did not render");
		}
		if (count == 64)
			Require(panel.SelectedTracker() == 63, "Tracker selection was clamped to four slots");
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Expanded tracker UI validation errors");
	std::cout << "PASS expanded tracker controls, empty inventory and complete spatial display\n";
}
void ConfigureObservedDevices()
{
	Interaction test;
	PlayerRig rig;
	DeviceControlPanel panel{rig};
	simxr::CommandConfiguration initial;
	initial.views.ipd = 0.073f;
	initial.views.outerWidth = 1760;
	initial.tracking.body.preset = simxr::BodyPreset::Seated;
	initial.tracking.gaze.yaw = 0.25f;
	initial.tracking.controllers[0].connected = false;
	simxr::CommandScheduler scheduler;
	std::optional<std::array<std::string, 2>> effectiveProfiles;
	scheduler.Start(0, initial);
	const auto observe = [&](std::uint64_t frame)
	{
		scheduler.Apply(simxr::FrameId{frame}, simxr::SimulationTime{simxr::Duration{100}},
						simxr::RuntimeTime{simxr::Duration{200}}, {});
		simxr::control::Message state;
		state.fields.push_back(simxr::control::Field::Bytes(
			simxr::control::tag::Document, simxr::control::SimulationState(scheduler.Snapshot(), false, {}, {}, effectiveProfiles).Encode()));
		panel.ObserveState(state);
	};
	observe(1);
	Selection selection = DeviceSel{DevicePage::Head};
	std::optional<SimulationEdit> edit;
	bool supported = true;
	bool advanced = false, showState = true, profileScreenshot = false;
	ImVec2 panelSize{400, 650};
	const auto frame = [&](bool headerOnly = false)
	{
		Eventually(
			[&]
			{
				return test.PollEvents() && test.renderer.BeginFrame();
			},
			"Live field UI stalled");
		test.workspace.BeginFrame(test.runtime);
		ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
		ImGui::SetNextWindowSize(panelSize, ImGuiCond_Always);
		ImGui::Begin("Configuration workflow");
		if (advanced)
			edit = panel.DrawAdvanced();
		else
		{
			const auto headerEdit = panel.DrawHeader(selection, showState);
			edit = headerOnly ? std::optional<SimulationEdit>{} : panel.Draw(supported, &selection);
			if (headerEdit)
				edit = headerEdit;
		}
		if (profileScreenshot)
		{
			Require(ImGui::GetItemRectMax().x <= ImGui::GetCurrentWindow()->InnerRect.Max.x,
				"Profile editor extends outside the visible inspector");
			ImGui::SetScrollHereY(1.0f);
		}
		ImGui::End();
		if (auto release = panel.EndInputFrame())
		{
			Require(!edit, "Configuration fixture produced concurrent edits");
			edit = std::move(release);
		}
		Require(test.renderer.Render(), "Live field frame did not render");
	};
	for (int i = 0; i < 3; ++i)
		frame();
	auto* window = FindPanel("Configuration workflow");
	const auto activate = [&](const char* label)
	{
		ImGui::SetWindowFocus(window->Name);
		ImGui::ActivateItemByID(window->GetID(label));
		frame();
		Require(edit && std::holds_alternative<ManualTransaction>(*edit),
				(std::string("Live field did not submit: ") + label).c_str());
		return std::get<ManualTransaction>(*edit);
	};
	const auto openTracking = [&]
	{
		ImGui::SetWindowFocus(window->Name);
		const auto page = static_cast<int>(std::get<DeviceSel>(selection).page);
		const auto scope = ImHashData(&page, sizeof(page), window->GetID("Tracking"));
		ImGui::ActivateItemByID(ImHashStr("Device state", 0, scope));
		frame(true);
		frame(true);
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Device state menu did not open");
		return popups.back().Window;
	};
	const auto tracking = [&](const char* label)
	{
		ImGui::ActivateItemByID(openTracking()->GetID(label));
		frame(true);
		Require(edit && std::holds_alternative<ManualTransaction>(*edit),
				(std::string("Tracking menu did not submit: ") + label).c_str());
		return std::get<ManualTransaction>(*edit);
	};
	const auto beginValue = [&](const char* label, const char* value)
	{
		ImGui::SetWindowFocus(window->Name);
		ImGui::ActivateItemByID(window->GetID(label));
		ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
		frame();
		auto& io = ImGui::GetIO();
		io.AddKeyEvent(ImGuiMod_Ctrl, true);
		io.AddKeyEvent(ImGuiKey_A, true);
		frame();
		io.AddKeyEvent(ImGuiKey_A, false);
		io.AddKeyEvent(ImGuiMod_Ctrl, false);
		frame();
		io.AddInputCharactersUTF8(value);
		frame();
		Require(edit && std::holds_alternative<ManualTransaction>(*edit), "Typing did not submit a live value");
		return std::get<ManualTransaction>(*edit);
	};
	const auto typeValue = [&](const char* label, const char* value)
	{
		const auto changed = beginValue(label, value);
		auto& io = ImGui::GetIO();
		io.AddKeyEvent(ImGuiKey_Enter, true);
		frame();
		io.AddKeyEvent(ImGuiKey_Enter, false);
		frame();
		return changed;
	};
	auto transaction = typeValue("##IPD", "0.075");
	const auto views = std::get<simxr::SetViewSettings>(transaction.commands.back()).value;
	Require(std::abs(views.ipd - .075f) < .0001f && views.outerWidth == 1760,
			"Live view edit overwrote unrelated observed configuration");
	panel.ObserveDelivery(false, "Owned by script");
	frame();
	bool rejectedBorder = false;
	for (const auto& vertex : window->DrawList->VtxBuffer)
		rejectedBorder |= vertex.col == IM_COL32(229, 72, 77, 255);
	Require(rejectedBorder, "Rejected field has no red feedback");
	transaction = typeValue("##Inset half angle", "12");
	Require(std::abs(std::get<simxr::SetViewSettings>(transaction.commands.back()).value.ipd - initial.views.ipd) <
				.0001f,
			"Rejected field did not revert to the observed value");
	selection = DeviceSel{DevicePage::Body};
	frame();
	transaction = activate("Body active");
	Require(std::get<simxr::SetBodyControls>(transaction.commands.back()).value.preset == simxr::BodyPreset::Seated,
			"Live body edit lost observed preset");
	selection = DeviceSel{DevicePage::Gaze};
	frame();
	transaction = activate("Gaze tracked");
	Require(std::get<simxr::SetGaze>(transaction.commands.back()).value.yaw == .25f, "Live gaze edit lost direction");
	selection = DeviceSel{DevicePage::Trackers};
	frame();
	transaction = activate("Tracked");
	Require(std::get<simxr::SetTracker>(transaction.commands.back()).value.identity ==
				initial.tracking.trackers[0].identity,
			"Live tracker edit changed identity");
	selection = DeviceSel{DevicePage::Head};
	frame();
	transaction = tracking("Lost");
	simxr::CommandTransaction command;
	command.id = 1;
	command.source = {simxr::CommandSourceKind::Manual, 77};
	command.target.frame = simxr::FrameId{2};
	command.commands = transaction.commands;
	Require(scheduler.Submit(command).result == simxr::CommandResult::Queued, "Tracking failure fixture was not admitted");
	panel.SetManualSource(command.source);
	observe(2);
	panel.ObserveDelivery(false, {});
	frame();
	transaction = tracking("Tracked");
	Require(!std::get<simxr::SetScenario>(transaction.commands[1]).enabled &&
				!std::get<simxr::SetScenario>(transaction.commands.back()).enabled,
			"Scenario recovery did not clear failure");
	command.id = 2;
	command.target.frame = simxr::FrameId{3};
	command.commands = transaction.commands;
	Require(scheduler.Submit(command).result == simxr::CommandResult::Queued, "Tracking recovery fixture was not admitted");
	observe(3);
	panel.ObserveDelivery(false, {});
	frame();
	showState = false;
	frame(true);
	const int headPage = static_cast<int>(DevicePage::Head);
	ImGui::ActivateItemByID(ImHashStr("Device state", 0,
		ImHashData(&headPage, sizeof(headPage), window->GetID("Tracking"))));
	frame(true);
	Require(!edit, "Historical header exposed live state controls");
	showState = true;
	if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
	{
		for (const auto page : {DevicePage::Head, DevicePage::Left, DevicePage::Body, DevicePage::Gaze,
							   DevicePage::Trackers})
		{
			selection = DeviceSel{page};
			frame();
			test.renderer.RequestScreenshot(directory / ("header-" + std::to_string(static_cast<int>(page)) + ".bmp"));
			frame();
		}
		selection = DeviceSel{DevicePage::Gaze};
		panelSize.x = 300;
		test.workspace.SetScale(1.5f);
		frame();
		frame();
		test.renderer.RequestScreenshot(directory / "header-narrow-large.bmp");
		frame();
		openTracking();
		test.renderer.RequestScreenshot(directory / "header-state-menu-large.bmp");
		frame(true);
		ImGui::ClosePopupsExceptModals();
		test.workspace.SetScale(1);
		panelSize.x = 400;
	}
	selection = DeviceSel{DevicePage::Head};
	panel.ResetObservations();
	frame();
	ImGui::ActivateItemByID(ImHashStr("Device state", 0,
		ImHashData(&headPage, sizeof(headPage), window->GetID("Tracking"))));
	frame();
	Require(!edit, "Disconnected field submitted stale settings");
	initial.views.ipd = .061f;
	scheduler.Start(0, initial);
	observe(1);
	frame();
	selection = DeviceSel{DevicePage::Head};
	frame();
	transaction = typeValue("##IPD", "0.063");
	Require(std::abs(std::get<simxr::SetViewSettings>(transaction.commands.back()).value.ipd - .063f) < .0001f,
			"Reconnected field did not send the new value");
	{
		auto& g = *ImGui::GetCurrentContext();
		auto& io = ImGui::GetIO();
		const auto hover = [&](const char* label)
		{
			const auto id = window->GetID(label);
			ImGui::SetFocusID(id, window);
			frame();
			Require(g.NavId == id && g.NavIdIsAlive, "Value field not laid out");
			const auto center = ImGui::WindowRectRelToAbs(window, window->NavRectRel[0]).GetCenter();
			ImGui::ClearActiveID();
			g.NavId = 0;
			SDL_Event event{};
			event.type = SDL_EVENT_MOUSE_MOTION;
			event.motion.windowID = SDL_GetWindowID(test.window.Native());
			event.motion.x = center.x;
			event.motion.y = center.y;
			Require(SDL_PushEvent(&event), "Push field pointer");
			frame();
		};
		const auto rightClick = [&](bool shift)
		{
			io.AddKeyEvent(ImGuiMod_Shift, shift);
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
			frame();
			io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
			frame();
		};
		const auto menuItem = [&](const char* label)
		{
			frame();
			Require(!g.OpenPopupStack.empty() && g.OpenPopupStack.back().Window, "Field context menu did not open");
			ImGui::ActivateItemByID(g.OpenPopupStack.back().Window->GetID(label));
			frame();
		};
		hover("##IPD");
		struct WheelCase
		{
			bool shift, control;
			float wheel, expected;
		};
		for (const auto& step : {WheelCase{false, false, -1, .062f}, {true, false, 1, .072f},
								{false, true, -1, .0719f}, {true, true, -1, .0718f}})
		{
			io.AddKeyEvent(ImGuiMod_Shift, step.shift);
			io.AddKeyEvent(ImGuiMod_Ctrl, step.control);
			const float scroll = window->Scroll.y;
			io.AddMouseWheelEvent(0, step.wheel);
			frame();
			Require(edit && std::holds_alternative<ManualTransaction>(*edit), "Wheel did not edit the field");
			const auto& nudged = std::get<simxr::SetViewSettings>(std::get<ManualTransaction>(*edit).commands.back());
			Require(std::abs(nudged.value.ipd - step.expected) < .00001f, "Wheel modifier used the wrong step");
			Require(window->Scroll.y == scroll, "Value wheel also scrolled the inspector");
		}
		io.AddKeyEvent(ImGuiMod_Ctrl, false);
		rightClick(false);
		menuItem("Copy");
		Require(!edit && std::abs(std::stof(ImGui::GetClipboardText()) - .0718f) < .00001f,
				"Copy rounded away the fine wheel adjustment");
		hover("##IPD");
		rightClick(true);
		Require(edit && std::holds_alternative<ManualTransaction>(*edit), "Shift+right-click did not reset");
		const auto reset = std::get<simxr::SetViewSettings>(std::get<ManualTransaction>(*edit).commands.back());
		Require(std::abs(reset.value.ipd - simxr::ViewSettings{}.ipd) < .0001f,
				"Shift+right-click did not restore the default IPD");
		Require(g.OpenPopupStack.empty(), "Reset gesture also opened a context menu");
		io.AddKeyEvent(ImGuiMod_Shift, false);
		frame();
		rightClick(false);
		menuItem("Copy");
		Require(!edit && std::abs(std::stof(ImGui::GetClipboardText()) - reset.value.ipd) < .0001f,
				"Field Copy changed input or copied the wrong value");
		window->StateStorage.SetInt(window->GetID("Position & rotation"), 1);
		frame();
		hover("##Yaw");
		rightClick(false);
		if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
		{
			test.renderer.RequestScreenshot(directory / "rotation-context.bmp");
			frame();
		}
		menuItem("Quaternion");
		Require(!edit, "Changing rotation display authored a pose");
		frame();
		hover("##Qx");
		io.AddMouseWheelEvent(0, 1);
		frame();
		Require(!edit, "Read-only quaternion accepted a wheel edit");
		rightClick(true);
		Require(!edit && g.OpenPopupStack.empty(), "Read-only quaternion accepted a reset gesture");
		io.AddKeyEvent(ImGuiMod_Shift, false);
		frame();
		rightClick(false);
		menuItem("Copy");
		Require(!edit && std::abs(std::stof(ImGui::GetClipboardText()) - rig.Head().orientation.x) < .00001f,
				"Quaternion Copy changed input or copied the wrong component");
		if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
		{
			test.workspace.SetScale(1.5f);
			panelSize.x = 480;
			frame();
			frame();
			hover("##Qx");
			rightClick(false);
			test.renderer.RequestScreenshot(directory / "quaternion-context-large.bmp");
			frame();
			ImGui::ClosePopupsExceptModals();
			test.workspace.SetScale(1);
			panelSize.x = 400;
			frame();
			frame();
		}
		hover("##Qx");
		rightClick(false);
		menuItem("Euler angles");
		Require(!edit, "Rotation context menu authored a pose");
		frame();
		hover("##Yaw");
		std::cout << "PASS value wheel modifiers, Shift+right-click reset, Copy and rotation display menus\n";
	}
	selection = DeviceSel{DevicePage::Head};
	frame();
	transaction = activate("Reset page");
	bool keptDisconnected = false;
	for (const auto& command : transaction.commands)
		if (const auto* pose = std::get_if<simxr::SetControllerPose>(&command); pose && pose->hand == 0)
			keptDisconnected = !pose->value.connected;
	Require(keptDisconnected, "Resetting Head reconnected the left controller");
	initial.input.controllers[1].trigger = .6f;
	initial.input.controllers[1].physicalProfile = "/interaction_profiles/htc/vive_controller";
	initial.input.controllers[1].triggerClick = false;
	effectiveProfiles = std::array<std::string, 2>{"", "/interaction_profiles/khr/simple_controller"};
	initial.input.controllers[1].grip = .4f;
	initial.input.controllers[1].stick = {.2f, -.3f};
	initial.input.controllers[1].primary = initial.input.controllers[1].secondary = true;
	initial.input.controllers[1].menu = initial.input.controllers[1].thumbClick = true;
	scheduler.Start(0, initial);
	panel.ResetObservations();
	observe(1);
	selection = DeviceSel{DevicePage::Right};
	for (const bool enabled : {true, false, true})
	{
		supported = enabled;
		frame();
		Require(!edit, "Idle or disabled input pad changed observed controls");
	}
	command.id = 1;
	command.source = {simxr::CommandSourceKind::Rig, 51};
	command.target.frame = simxr::FrameId{2};
	command.commands = {simxr::TakeControl{simxr::ControlChannel::RightControls, true},
						simxr::SetControllerControls{1, initial.input.controllers[1]}};
	Require(scheduler.Submit(command).result == simxr::CommandResult::Queued, "Pad owner fixture was not admitted");
	observe(2);
	Require(scheduler.Snapshot().applied->owners[static_cast<std::size_t>(simxr::ControlChannel::RightControls)] ==
		command.source, "Pad owner fixture did not acquire the controls channel");
	panel.SetManualSource(simxr::CommandSource{simxr::CommandSourceKind::Manual, 77});
	frame();
	Require(!edit, "Other-owned input pad submitted an unsolicited edit");
	if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
	{
		window->StateStorage.SetInt(window->GetID("Connection and profile"), 1);
		for (const float scale : {1.0f, 1.5f})
		{
			test.workspace.SetScale(scale);
			panelSize.x = scale == 1 ? 400 : 320 * scale;
			profileScreenshot = true;
			frame();
			frame();
			frame();
			test.renderer.RequestScreenshot(directory / (scale == 1 ? "physical-profiles.bmp" : "physical-profiles-narrow.bmp"));
			frame();
		}
		test.workspace.SetScale(1);
		panelSize.x = 400;
		profileScreenshot = false;
		window->StateStorage.SetInt(window->GetID("Connection and profile"), 0);
		ImGui::SetScrollY(window, 0);
		frame();
	}

	ImGui::ActivateItemByID(ImHashStr("Primary", 0, window->GetID(1)));
	frame();
	Require(!edit, "Other-owned input pad accepted a press before takeover");
	ImGui::ActivateItemByID(ImHashStr("Take over##Input", 0, window->GetID(1)));
	frame();
	Require(edit && std::holds_alternative<ManualTransaction>(*edit), "Input pad takeover did not submit");
	const auto& takeover = std::get<ManualTransaction>(*edit);
	Require(takeover.commands.size() == 1 && std::holds_alternative<simxr::TakeControl>(takeover.commands.front()) &&
				std::get<simxr::TakeControl>(takeover.commands.front()).replace,
			"Input pad takeover changed input before ownership acknowledgement");
	std::cout << "PASS passive/disabled input-pad observation and explicit ownership takeover\n";
	initial.tracking.trackers[0].role = simxr::TrackerRole::Camera;
	initial.tracking.trackers[1].role = simxr::TrackerRole::Keyboard;
	initial.tracking.trackers[0].jointOffset = XrPosef{{0, 0, 0, 1}, {2, 0, 0}};
	initial.tracking.trackers[1].jointOffset = XrPosef{{0, 0, 0, 1}, {3, 0, 0}};
	scheduler.Start(0, initial);
	panel.ResetObservations();
	observe(1);
	selection = DeviceSel{DevicePage::Trackers, 0};
	window->StateStorage.SetInt(window->GetID("Role"), 1);
	frame();
	selection = DeviceSel{DevicePage::Trackers, 1};
	transaction = activate("Tracked");
	const auto selectedTracker = std::get<simxr::SetTracker>(transaction.commands.back());
	Require(selectedTracker.index == 1 && selectedTracker.value.role == simxr::TrackerRole::Keyboard &&
				!selectedTracker.value.tracked,
			"External tracker selection reused the previous slot's role");
	panel.ObserveDelivery(false, {});
	selection = DeviceSel{DevicePage::Trackers, 0};
	frame();
	Require(std::abs(rig.TrackerAt(0).offset.position.x - 2) < .0001f &&
				std::abs(rig.TrackerAt(1).offset.position.x - 3) < .0001f,
			"Tracker reset fixture lost its distinct offsets");
	selection = DeviceSel{DevicePage::Trackers, 1};
	transaction = activate("Reset page");
	Require(std::abs(rig.TrackerAt(0).offset.position.x - 2) < .0001f &&
				std::abs(rig.TrackerAt(1).offset.position.x) < .0001f,
			"Tracker header reset used the previously selected slot");
	scheduler.Start(0, initial);
	panel.ResetObservations();
	observe(1);
	panel.SetManualSource(simxr::CommandSource{simxr::CommandSourceKind::Manual, 77});
	command.id = 1;
	command.source = {simxr::CommandSourceKind::Rig, 51};
	command.target.frame = simxr::FrameId{2};
	const simxr::ControlChannel ownedChannels[] = {simxr::ControlChannel::Head, simxr::ControlChannel::Gaze,
													 simxr::ControlChannel::Trackers, simxr::ControlChannel::Views};
	command.commands.clear();
	for (const auto channel : ownedChannels)
		command.commands.emplace_back(simxr::TakeControl{channel, true});
	Require(scheduler.Submit(command).result == simxr::CommandResult::Queued, "Page owner fixture was not admitted");
	observe(2);
	for (const auto channel : ownedChannels)
		Require(scheduler.Snapshot().applied->owners[static_cast<std::size_t>(channel)] == command.source,
				"Page owner fixture did not acquire its channel");
	advanced = true;
	frame();
	ImGui::ActivateItemByID(window->GetID("Replace owner"));
	frame();
	Require(!edit, "Changing the Advanced replacement option submitted an edit");
	transaction = activate("Take control");
	Require(transaction.commands.size() == 1 && std::get<simxr::TakeControl>(transaction.commands.front()).replace,
			"Advanced replacement fixture did not enable explicit takeover");
	advanced = false;
	panel.ObserveDelivery(false, {});
	const auto blockedAction = [&](const char* label, bool numeric = false)
	{
		ImGui::ActivateItemByID(window->GetID(label));
		if (numeric)
			ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
		frame();
		Require(!edit && !panel.Active(),
				(std::string("Other-owned field accepted an ordinary edit: ") + label).c_str());
	};
	selection = DeviceSel{DevicePage::Gaze};
	frame();
	blockedAction("Gaze tracked");
	blockedAction("Reset page");
	selection = DeviceSel{DevicePage::Head};
	frame();
	ImGui::ActivateItemByID(ImHashStr("Device state", 0,
		ImHashData(&headPage, sizeof(headPage), window->GetID("Tracking"))));
	frame();
	Require(!edit && ImGui::GetCurrentContext()->OpenPopupStack.empty(),
			"Other-owned device opened its state menu");
	blockedAction("##X", true);
	blockedAction("##IPD", true);
	blockedAction("Reset page");
	selection = DeviceSel{DevicePage::Trackers, 1};
	frame();
	blockedAction("Tracked");
	blockedAction("Snap to body");
	blockedAction("Reset page");
	command.id = 2;
	command.target.frame = simxr::FrameId{3};
	command.commands = {simxr::ReleaseControl{simxr::ControlChannel::Views}};
	Require(scheduler.Submit(command).result == simxr::CommandResult::Queued, "View release fixture was not admitted");
	observe(3);
	Require(!scheduler.Snapshot().applied->owners[static_cast<std::size_t>(simxr::ControlChannel::Views)],
			"View release fixture retained its owner");
	selection = DeviceSel{DevicePage::Head};
	frame();
	beginValue("##IPD", "0.069");
	Require(panel.Active(), "Takeover fixture did not retain its numeric edit");
	ImGui::ActivateItemByID(ImHashStr("Take over", 0, window->GetID(static_cast<int>(simxr::ControlChannel::Head))));
	frame();
	Require(edit && std::holds_alternative<ManualTransaction>(*edit), "Head takeover did not submit");
	transaction = std::get<ManualTransaction>(*edit);
	Require(transaction.commands.size() == 1 && std::holds_alternative<simxr::TakeControl>(transaction.commands.front()) &&
				std::get<simxr::TakeControl>(transaction.commands.front()).channel == simxr::ControlChannel::Head &&
				std::get<simxr::TakeControl>(transaction.commands.front()).replace,
			"Numeric field finalization replaced the explicit Head takeover");
	scheduler.Start(0, initial);
	panel.ResetObservations();
	observe(1);
	selection = DeviceSel{DevicePage::Head};
	frame();
	beginValue("##IPD", "0.068");
	Require(panel.Active(), "Numeric field fixture did not retain its active edit");
	supported = false;
	frame();
	Require(!edit && !panel.Active(), "Disabled numeric field submitted a stale final value");
	supported = true;
	panel.ObserveDelivery(false, {});
	selection = DeviceSel{DevicePage::Gaze};
	frame();
	beginValue("##Yaw", "30");
	Require(panel.Active(), "Gaze field fixture did not retain its active edit");
	selection = DeviceSel{DevicePage::Head};
	frame();
	Require(!edit && !panel.Active(), "Device selection transferred a numeric edit to the matching field ID");
	frame();
	Require(!edit, "Retired numeric field submitted a delayed final value");
	auto limitedDocument = simxr::control::SimulationState(scheduler.Snapshot(), false, {}, {}, effectiveProfiles).Encode();
	const auto omitExtensions = [&](auto&& self, std::vector<std::byte>& encoded) -> void
	{
		auto node = simxr::control::DecodeFields(encoded).fields;
		const auto type = static_cast<simxr::control::InspectionType>(*node[1].AsU32());
		if (type == simxr::control::InspectionType::Object)
		{
			auto children = simxr::control::DecodeFields(node[2].value).fields;
			std::erase_if(children, [](const auto& child)
			{
				const auto fields = simxr::control::DecodeFields(child.value).fields;
				const auto name = fields.front().AsText();
				return name == "joints" || name == "pose_override";
			});
			for (auto& child : children)
				self(self, child.value);
			node[2].value = simxr::control::EncodeFields(children);
		}
		else if (type == simxr::control::InspectionType::Array)
		{
			auto pages = simxr::control::DecodeFields(node[2].value).fields;
			for (auto& page : pages)
			{
				auto children = simxr::control::DecodeFields(page.value).fields;
				for (auto& child : children)
					self(self, child.value);
				page.value = simxr::control::EncodeFields(children);
			}
			node[2].value = simxr::control::EncodeFields(pages);
		}
		encoded = simxr::control::EncodeFields(node);
	};
	omitExtensions(omitExtensions, limitedDocument);
	simxr::control::Message limitedState;
	limitedState.fields.push_back(simxr::control::Field::Bytes(simxr::control::tag::Document, limitedDocument));
	const auto limitedSetup = ReadObservedSetup(limitedState);
	Require(!limitedSetup.bodyOverridesAvailable && !limitedSetup.trackerAttachmentsAvailable,
			"Limited runtime fixture still advertised optional device editing");
	panel.ResetObservations();
	panel.ObserveState(limitedState);
	for (const auto page : {DevicePage::Body, DevicePage::Trackers})
	{
		selection = DeviceSel{page};
		frame();
		ImGui::ActivateItemByID(window->GetID("Reset page"));
		frame();
		Require(!edit, "Unsupported device reset submitted edits to unrelated channels");
	}
	std::cout << "PASS device field ownership, tracker selection/reset and numeric gesture retirement\n";
	RuntimeView statusView;
	statusView.connected = true;
	auto& statusSetup = statusView.setup.emplace();
	statusSetup.values.tracking.head.flags = simxr::TrackedPoseFlags;
	for (auto& controller : statusSetup.values.tracking.controllers)
	{
		controller.connected = true;
		controller.pose.flags = simxr::TrackedPoseFlags;
	}
	statusSetup.values.tracking.body.active = true;
	statusSetup.values.tracking.gaze.available = statusSetup.values.tracking.gaze.tracked = true;
	statusSetup.values.tracking.trackers[0].connected = statusSetup.values.tracking.trackers[0].tracked = true;
	CompanionSettings statusSettings;
	std::array<ImU32, 7> dotColors{};
	test.extraDraw = [&]
	{
		ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({560, 100}, ImGuiCond_Always);
		ImGui::Begin("Observed device status");
		const auto origin = ImGui::GetCursorScreenPos();
		const float width = ImGui::GetContentRegionAvail().x / dotColors.size();
		const float unit = ImGui::GetFontSize() / 17;
		DrawDeviceTabs(FrameSel{}, statusView, statusSettings);
		const auto& vertices = ImGui::GetWindowDrawList()->VtxBuffer;
		dotColors.fill(0);
		for (std::size_t index = 1; index < dotColors.size(); ++index)
		{
			const ImVec2 dot{origin.x + width * (index + .5f) + 8 * unit,
							 origin.y + DeviceTabsHeight() / 2 + 6 * unit};
			for (int vertex = vertices.Size - 1; vertex >= 0; --vertex)
			{
				const auto& point = vertices[vertex];
				const float dx = point.pos.x - dot.x, dy = point.pos.y - dot.y;
				if ((point.col & IM_COL32_A_MASK) == IM_COL32_A_MASK && dx * dx + dy * dy < 16 * unit * unit)
				{
					dotColors[index] = point.col;
					break;
				}
			}
		}
		ImGui::End();
	};
	for (const auto failure : {simxr::FailureScenario::HeadUntracked, simxr::FailureScenario::BodyUntracked,
							   simxr::FailureScenario::GazeUntracked})
		statusSetup.scenarios[static_cast<std::size_t>(failure)] = true;
	test.Frame();
	test.Frame();
	for (const auto page : {DevicePage::Head, DevicePage::Body, DevicePage::Gaze})
		Require(dotColors[static_cast<std::size_t>(page)] == theme::Warn,
				"Lost scenario did not change the device status dot");
	for (const auto failure : {simxr::FailureScenario::HeadUnavailable, simxr::FailureScenario::LeftDisconnected,
							   simxr::FailureScenario::RightDisconnected, simxr::FailureScenario::BodyUnavailable,
							   simxr::FailureScenario::GazeUnavailable, simxr::FailureScenario::TrackersDisconnected})
		statusSetup.scenarios[static_cast<std::size_t>(failure)] = true;
	test.Frame();
	for (std::size_t index = 1; index < dotColors.size(); ++index)
		Require(dotColors[index] == theme::TextDim, "Off scenario did not override the device status dot");
	statusSetup.scenarios.fill(false);
	test.Frame();
	for (std::size_t index = 1; index < dotColors.size(); ++index)
		Require(dotColors[index] == theme::Ok, "Scenario recovery did not restore the tracked device status dot");
	test.extraDraw = {};
	std::cout << "PASS device status dots follow Lost/Off scenarios and recovery\n";
	test.renderer.WaitIdle();
	Require(!test.validation->errors.load(), "Live fields generated validation errors");
	std::cout << "PASS live configuration: immediate changes, preserved configuration, scenario recovery, reconnect, "
				 "Shift+right-click reset, normal/fast/fine wheel steps\n";
}

}
