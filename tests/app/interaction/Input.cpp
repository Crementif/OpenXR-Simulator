#include "app/UiPch.h"
#include "Cases.h"
#include "app/services/PlayerRig.h"
#include "app/services/ManualInputController.h"
#include "app/services/SimulationEdits.h"
#include "app/services/SpatialMath.h"
#include "app/services/InputHistory.h"
#include "app/services/RecordingController.h"
#include "app/services/RuntimeConnection.h"
#include "app/widgets/PilotController.h"
#include "app/widgets/SpatialEditor.h"
#include "app/widgets/TimelineCanvas.h"
#include "app/widgets/AppPicker.h"
#include "app/widgets/Theme.h"
#include "app/services/Utf8Path.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include <iostream>
#include <random>
#ifdef __linux__
#include <unistd.h>
#endif

namespace simxr::tests
{
#ifdef __linux__
void CheckSpatialEditing(Interaction& test)
{
	IgnoreRealPointer();
	const auto target = test.runtime.Snapshot();
	Require(target.selectedViews.size() == 2, "Displayed image omitted exact compositor views");
	ManualInputController observer;
	observer.SetTarget({target.endpoint, target.session, target.generation});
	const auto pump = [&](int count) {
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto observe = [&] {
		const auto previous = observer.Snapshot().observation;
		observer.Refresh();
		Eventually(
			[&] {
				pump(1);
				return observer.Snapshot().observation > previous;
			},
			"Spatial observation stalled");
		return ReadObservedSetup(observer.Snapshot().state);
	};
	const auto pointer = [&](ImVec2 position, int button) {
		SDL_WarpMouseInWindow(test.window.Native(), position.x, position.y);
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(test.window.Native());
		event.motion.which = TestMouse;
		event.motion.x = position.x;
		event.motion.y = position.y;
		Require(SDL_PushEvent(&event), "Push spatial pointer");
		if (button >= 0)
		{
			event = {};
			event.type = button ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.windowID = SDL_GetWindowID(test.window.Native());
			event.button.which = TestMouse;
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = button != 0;
			Require(SDL_PushEvent(&event), "Push spatial button");
		}
		pump(1);
	};
	const auto drag = [&](ImVec2 from, ImVec2 to) {
		pointer(from, 1);
		for (int step = 1; step <= 6; ++step)
			pointer({from.x + (to.x - from.x) * step / 6, from.y + (to.y - from.y) * step / 6}, -1);
		pointer(to, 0);
		pump(8);
	};
	SDL_ShowWindow(test.window.Native());
	SDL_RaiseWindow(test.window.Native());
	test.window.Resize(1920, 1080);
	SDL_WarpMouseInWindow(test.window.Native(), 960, 540);
	test.workspace.SetScale(1);
	test.workspace.Select(DeviceSel{DevicePage::Right});
	Eventually(
		[&] {
			pump(1);
			return test.window.Focused();
		},
		"Spatial window did not receive focus");
	pump(6);
	auto before = observe();
	PlayerRig rig;
	rig.Reset(before);
	auto after = before;
	const auto ball = test.runtime.Snapshot().rotationBall;
	Require(ball.width > 100, "Rotation ball missing");
	const ImVec2 resetPoint{ball.x + ball.width * .65f, ball.y + ball.height * .60f};
	pointer(resetPoint, 1);
	pointer(resetPoint, 0);
	pointer(resetPoint, 1);
	pointer(resetPoint, 0);
	pump(8);
	after = observe();
	const auto inspector = FindPanel("Inspector");
	Require(inspector, "Spatial inspector missing");
	ChooseRigView(inspector->GetID("Rig view"), "Top", pump);
	const auto facing = Rotate(rig.Root().orientation, {0, 0, -1});
	const auto view = EulerAngles(std::atan2(-facing.x, -facing.z), -1.56f, 0);
	const auto local =
		RelativePose({after.values.tracking.controllers[1].pose.pose.orientation, {}}, {view, {}}).orientation;
	const auto forward = Rotate(local, {0, 0, -1});
	const ImVec2 tip{ball.x + ball.width / 2 + forward.x * ball.width * .46f * .75f,
					 ball.y + ball.height / 2 - forward.y * ball.width * .46f * .75f};
	pointer(tip, 1);
	pointer(tip, 0);
	pump(8);
	after = observe();
	if (Rotate(after.values.tracking.controllers[1].pose.pose.orientation, {0, 0, -1}).y <= .99f)
	{
		if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(directory) / "spatial-orb-failure.bmp");
			pump(1);
		}
		std::cout << "Spatial orb focus=" << test.window.Focused() << " tip=" << tip.x << "," << tip.y
				  << " up=" << Rotate(after.values.tracking.controllers[1].pose.pose.orientation, {0, 0, -1}).y << '\n';
	}
	Require(Rotate(after.values.tracking.controllers[1].pose.pose.orientation, {0, 0, -1}).y > .99f,
			"Rotation ball did not point the hand up");
	std::cout << "PASS arcball pointing up through native observed state\n";
	const auto changesBefore = test.runtime.Changes().size();
	const ImVec2 orbStart{ball.x + ball.width * .32f, ball.y + ball.height * .32f};
	const ImVec2 orbEnd{orbStart.x + 18, orbStart.y + 22};
	pointer(orbStart, 1);
	pointer(orbEnd, -1);
	pump(8);
	const auto interrupted = observe().values.tracking.controllers[1].pose.pose;
	Require(std::abs(RelativePose(interrupted, after.values.tracking.controllers[1].pose.pose).orientation.w) < .9999f,
			"Interrupted rotation fixture did not move the right hand");
	test.workspace.SetPreviewExpanded(true);
	pump(2);
	pointer(orbEnd, 0);
	test.workspace.Select(DeviceSel{DevicePage::Left});
	test.workspace.SetPreviewExpanded(false);
	pump(4);
	const auto leftBefore = observe().values.tracking.controllers[0].pose.pose;
	const auto leftBall = test.runtime.Snapshot().rotationBall;
	drag({leftBall.x + leftBall.width * .32f, leftBall.y + leftBall.height * .32f},
		 {leftBall.x + leftBall.width * .32f + 18, leftBall.y + leftBall.height * .32f + 22});
	const auto editedLeft = observe().values.tracking.controllers[0].pose.pose;
	Require(std::abs(RelativePose(editedLeft, leftBefore).orientation.w) < .9999f,
			"Second rotation fixture did not move the left hand");
	Require(test.runtime.Changes().size() == changesBefore + 2,
			"Interrupted rotation merged with a different device's undo gesture");
	test.runtime.UndoChanges(1);
	pump(8);
	const auto undone = observe();
	Require(
		std::abs(RelativePose(undone.values.tracking.controllers[0].pose.pose, leftBefore).orientation.w) > .9999f &&
			std::abs(RelativePose(undone.values.tracking.controllers[1].pose.pose, interrupted).orientation.w) > .9999f,
		"Undo after a hidden rotation changed the wrong gesture");
	test.runtime.UndoChanges(1);
	pump(8);
	test.workspace.Select(DeviceSel{DevicePage::Right});
	pump(4);
	std::cout << "PASS hidden rotation retirement and independent undo after device selection\n";
	ImGui::SetScrollY(inspector, inspector->ScrollMax.y);
	pump(2);
	ImGui::ActivateItemByID(inspector->GetID("Position & rotation"));
	pump(2);
	ImGui::SetScrollY(inspector, inspector->ScrollMax.y);
	pump(2);
	ImGui::ActivateItemByID(inspector->GetID("World##Relative"));
	pump(2);
	ImGui::ActivateItemByID(inspector->GetID("##X"));
	ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	pump(1);
	Require(ImGui::GetActiveID() == inspector->GetID("##X"), "Device position field did not activate");
	const auto activeChanges = test.runtime.Changes().size();
	Require(activeChanges != 0, "Active-field undo fixture has no prior gesture");
	test.runtime.UndoChanges(1);
	Require(test.runtime.Changes().size() == activeChanges && ImGui::GetActiveID() == inspector->GetID("##X"),
			"Undo changed history while a device field was active");
	auto& io = ImGui::GetIO();
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiKey_A, true);
	pump(1);
	io.AddKeyEvent(ImGuiKey_A, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	pump(1);
	io.AddInputCharactersUTF8("8");
	pump(1);
	io.AddKeyEvent(ImGuiKey_Enter, true);
	pump(1);
	io.AddKeyEvent(ImGuiKey_Enter, false);
	pump(4);
	ImGui::ActivateItemByID(inspector->GetID("Body##Relative"));
	pump(2);
	after = observe();
	std::cout << "Spatial QA off-screen applied="
			  << RelativePose(after.values.tracking.controllers[1].pose.pose, rig.Root()).position.x << "\n";
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(directory) / "spatial-offscreen.bmp");
		pump(1);
		test.renderer.WaitIdle();
	}
	Require(RelativePose(after.values.tracking.controllers[1].pose.pose, rig.Root()).position.x > 5,
			"Off-screen test hand did not move");
	pump(10);
	const auto marker = test.runtime.Snapshot().markers[2];
	Require(marker.width > 0, "Off-screen recovery marker missing");
	const ImVec2 arrow{marker.x + marker.width / 2, marker.y + marker.height / 2};
	pointer(arrow, 1);
	pointer(arrow, 0);
	pump(8);
	after = observe();
	const auto hand = RelativePose(after.values.tracking.controllers[1].pose.pose, rig.Root());
	Require(std::abs(hand.position.x - .2f) < .015f && std::abs(hand.position.z + .55f) < .015f,
			("One-click off-screen recovery did not apply Forward: hand at " + std::to_string(hand.position.x) + ", " +
			 std::to_string(hand.position.z) + "; arrow at " + std::to_string(arrow.x) + ", " + std::to_string(arrow.y))
				.c_str());
	std::cout << "PASS one-click off-screen recovery\n";
	const auto gizmoState = test.runtime.Snapshot();
	const auto world = after.values.tracking.controllers[1].pose.pose;
	const auto projected = ProjectPoint(gizmoState.selectedViews.front(), world.position);
	const auto xAxis =
		ProjectPoint(gizmoState.selectedViews.front(), {world.position.x + .1f, world.position.y, world.position.z});
	const float length = std::hypot(xAxis.uv.x - projected.uv.x, xAxis.uv.y - projected.uv.y);
	const ImVec2 direction{(xAxis.uv.x - projected.uv.x) / length, (xAxis.uv.y - projected.uv.y) / length};
	const auto handle = gizmoState.markers[2];
	const ImVec2 origin{handle.x + handle.width / 2, handle.y + handle.height / 2};
	drag({origin.x + direction.x * 28, origin.y + direction.y * 28},
		 {origin.x + direction.x * 65, origin.y + direction.y * 65});
	after = observe();
	Require(after.values.tracking.controllers[1].pose.pose.position.x > world.position.x + .01f,
			"Preview gizmo did not translate the right hand");
	std::cout << "PASS preview gizmo translation through native observed state\n";
	const auto leftPose = after.values.tracking.controllers[0].pose.pose;
	const auto retargetMarker = test.runtime.Snapshot().markers[2];
	const ImVec2 retargetOrigin{retargetMarker.x + retargetMarker.width / 2,
								retargetMarker.y + retargetMarker.height / 2};
	const ImVec2 heldHandle{retargetOrigin.x + direction.x * 45, retargetOrigin.y + direction.y * 45};
	pointer({retargetOrigin.x + direction.x * 28, retargetOrigin.y + direction.y * 28}, 1);
	pointer(heldHandle, -1);
	pump(4);
	Require(observe().values.tracking.controllers[1].pose.pose.position.x >
				after.values.tracking.controllers[1].pose.pose.position.x + .005f,
			"Retarget fixture did not begin a gizmo drag");
	test.workspace.Select(DeviceSel{DevicePage::Left});
	pump(2);
	pointer({heldHandle.x + direction.x * 35, heldHandle.y + direction.y * 35}, -1);
	pump(4);
	const auto retargeted = observe().values.tracking.controllers[0].pose.pose;
	Require(std::hypot(retargeted.position.x - leftPose.position.x, retargeted.position.y - leftPose.position.y,
					   retargeted.position.z - leftPose.position.z) < .001f,
			"Changing selection transferred an active gizmo to another device");
	pointer(heldHandle, 0);
	test.workspace.Select(DeviceSel{DevicePage::Right});
	pump(3);
	std::cout << "PASS preview gizmo cancellation before device retargeting\n";
	ImGui::ActivateItemByID(ImHashStr("Expand", 0, inspector->GetID("Rig view")));
	pump(3);
	Require(test.runtime.Snapshot().rigExpanded, "Rig view did not expand");
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(directory) / "spatial-expanded.bmp");
		pump(1);
	}
	const auto imageArea = FindPanel("Image area");
	Require(imageArea, "Expanded rig view missing");
	ImGui::ActivateItemByID(ImHashStr("Expand", 0, imageArea->GetID("Rig view")));
	pump(3);
	Require(!test.runtime.Snapshot().rigExpanded, "Rig view did not restore the image");
	std::cout << "PASS expanded rig view and picture-in-picture restore\n";
	test.runtime.PrepareReplay();
	Eventually(
		[&] {
			pump(1);
			return test.runtime.ReplayReady();
		},
		"Replay input release did not complete");
	test.runtime.PrepareReplay();
	Require(!test.runtime.ReplayReady(), "Replay used the observation from before its release request");
	Eventually(
		[&] {
			pump(1);
			return test.runtime.ReplayReady();
		},
		"Replay did not observe its new input release");
	test.runtime.FinishReplayPreparation();
	std::cout << "PASS active device field blocks undo and replay waits for a fresh release observation\n";

	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		for (float scale : {1.0f, 1.5f})
		{
			test.workspace.SetScale(scale);
			pump(4);
			test.renderer.RequestScreenshot(
				(std::filesystem::path(directory) / (scale == 1 ? "spatial-100.bmp" : "spatial-150.bmp")));
			pump(1);
		}
		test.renderer.WaitIdle();
	}
	Require(test.validation->errors.load() == 0, "Spatial preview validation errors");
}

static void DrivePilotPulses(Interaction& test, const std::function<void(int)>& onPulse = {})
{
	const auto pump = [&](int count) {
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto push = [&](SDL_Event event) {
		Require(SDL_PushEvent(&event), "Push take input");
		pump(1);
	};
	const auto button = [&](unsigned code, bool down) {
		SDL_Event event{};
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.windowID = SDL_GetWindowID(test.window.Native());
		event.button.which = TestMouse;
		event.button.button = code;
		event.button.down = down;
		if (code == SDL_BUTTON_RIGHT && down)
		{
			const auto center = FindPanel("Image area")->InnerRect.GetCenter();
			SDL_WarpMouseInWindow(test.window.Native(), center.x, center.y);
			SDL_Event pointer{};
			pointer.type = SDL_EVENT_MOUSE_MOTION;
			pointer.motion.which = TestMouse;
			pointer.motion.windowID = event.button.windowID;
			pointer.motion.x = center.x;
			pointer.motion.y = center.y;
			push(pointer);
		}
		push(event);
	};
	const auto key = [&](SDL_Scancode code, bool down) {
		SDL_Event event{};
		event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.windowID = SDL_GetWindowID(test.window.Native());
		event.key.scancode = code;
		event.key.key = SDL_GetKeyFromScancode(code, SDL_KMOD_NONE, false);
		event.key.down = down;
		push(event);
	};
	Require(SDL_ShowWindow(test.window.Native()), "Show take window");
	SDL_RaiseWindow(test.window.Native());
	Eventually(
		[&] {
			pump(1);
			return test.window.Focused();
		},
		"Take window did not receive focus");
	pump(3);
	button(SDL_BUTTON_RIGHT, true);
	Eventually(
		[&] {
			pump(1);
			return SDL_GetWindowRelativeMouseMode(test.window.Native());
		},
		"Take pilot did not capture pointer");
	for (int pulse = 0; pulse < 4; ++pulse)
	{
		if (onPulse)
			onPulse(pulse);
		button(SDL_BUTTON_LEFT, true);
		pump(15);
		button(SDL_BUTTON_LEFT, false);
		key(SDL_SCANCODE_R, true);
		pump(10);
		key(SDL_SCANCODE_R, false);
		pump(10);
	}
	key(SDL_SCANCODE_ESCAPE, true);
	key(SDL_SCANCODE_ESCAPE, false);
	button(SDL_BUTTON_RIGHT, false);
	pump(10);
}

void CheckInputTake(Interaction& test, const std::filesystem::path& directory)
{
	const auto pump = [&](int count) {
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto& history = test.runtime.History();
	// A second client drives body, gaze and one tracker while the pilot presses
	// buttons, so the cut take must carry every device channel.
	const auto target = test.runtime.Snapshot();
	ManualInputController devices;
	devices.SetTarget({target.endpoint, target.session, target.generation});
	Eventually(
		[&] {
			pump(1);
			return devices.Snapshot().observation != 0;
		},
		"Device client did not connect");
	auto tracking = ReadObservedSetup(devices.Snapshot().state).values.tracking;
	const auto submitDevices = [&](int pulse) {
		tracking.body.preset = pulse >= 1 ? simxr::BodyPreset::TPose : simxr::BodyPreset::Standing;
		tracking.gaze.yaw = pulse >= 2 ? .3f : 0;
		tracking.trackers[0].jointOffset.reset();
		tracking.trackers[0].poseOverride = XrPosef{{0, 0, 0, 1}, {0.1f * float(pulse), 1.0f, -0.3f}};
		ManualTransaction edit;
		edit.commands = {
			simxr::TakeControl{simxr::ControlChannel::Body, false},		simxr::SetBodyControls{tracking.body},
			simxr::TakeControl{simxr::ControlChannel::Gaze, false},		simxr::SetGaze{tracking.gaze},
			simxr::TakeControl{simxr::ControlChannel::Trackers, false}, simxr::SetTracker{0, tracking.trackers[0]}};
		Require(devices.Submit(edit), "Device edit refused");
		Eventually(
			[&] {
				pump(1);
				const auto s = devices.Snapshot();
				Require(s.error.empty(), s.error.c_str());
				return !s.editPending;
			},
			"Device edit did not apply");
	};
	submitDevices(0);
	pump(20);
	const auto begin = history.Size();
	DrivePilotPulses(test, submitDevices);
	Require(devices.Submit(ReleaseManualInputs{}), "Device release refused");
	Eventually(
		[&] {
			pump(1);
			return !devices.Snapshot().editPending;
		},
		"Device release did not apply");
	Require(history.Size() > begin + 60, "Too few frames while driving input");
	std::size_t pressed = 0;
	for (auto index = begin; index < history.Size(); ++index)
		pressed += history.At(index).hands[1] > 200;
	Require(pressed > 20, "Input lane has no right-hand presses");
	const auto& first = history.At(begin);
	const auto& last = history.At(history.Size() - 1);
	const auto sourceCount = history.Size() - begin;
	test.workspace.Select(RangeSel{first.identity, last.identity, first.epoch});
	pump(3);
	auto* inspector = FindPanel("Inspector");
	Require(inspector, "Inspector missing for the range page");
	ImGui::ActivateItemByID(inspector->GetID("Save as take"));
	pump(2);
	const auto* notices = FindPanel("##Notices");
	Require(notices, "Save as take showed no notice");
	const auto recordings = directory / "recordings";
	std::filesystem::path take;
	for (const auto& entry : std::filesystem::directory_iterator(recordings))
		if (entry.path().extension() == ".luau")
			take = entry.path();
	Require(!take.empty(), "Save as take wrote no file");
	const auto readTake = [&] {
		std::ifstream input(take, std::ios::binary);
		Require(input.good(), "Could not read the saved range take");
		return std::string(std::istreambuf_iterator<char>(input), {});
	};
	const auto savedTake = readTake();
	{
		std::ofstream existing(take, std::ios::binary | std::ios::app);
		existing << "\n-- existing range take marker\n";
		existing.close();
		Require(existing.good(), "Could not mark the existing range take");
	}
	const auto existingTake = readTake();
	ImGui::ActivateItemByID(inspector->GetID("Save as take"));
	pump(2);
	Require(readTake() == existingTake, "Save as take replaced an existing file without the Replace action");
	ImGui::ActivateItemByID(inspector->GetID("Replace take"));
	pump(2);
	Require(readTake() == savedTake, "Replace take did not commit the selected range over the existing file");
	std::cout << "PASS saved " << history.Size() - begin << " frames as " << take.filename() << '\n';
	ImGui::ActivateItemByID(inspector->GetID("Zoom to range"));
	pump(2);

	const auto replayFrom = history.Size();
	{
		const auto& shell = test.workspace.State();
		const auto canvas = shell.timelineCanvas;
		const float ruler = ImGui::GetFrameHeight() + 2 * ImGui::GetFontSize() / 16;
		const float lane = (canvas.height - ruler) / 3;
		const float tab = std::min(ImGui::GetFontSize() * (1 + 4.0f / 16), lane);
		const float x =
			canvas.x +
			float((first.seconds - shell.timeline.first) / (shell.timeline.last - shell.timeline.first)) * canvas.width;
		SDL_Event motion{};
		motion.type = SDL_EVENT_MOUSE_MOTION;
		motion.motion.which = TestMouse;
		motion.motion.windowID = SDL_GetWindowID(test.window.Native());
		motion.motion.x = std::max(canvas.x, x) + tab / 2;
		motion.motion.y = canvas.y + ruler + tab / 2;
		SDL_WarpMouseInWindow(test.window.Native(), motion.motion.x, motion.motion.y);
		Require(SDL_PushEvent(&motion), "Hover the take clip");
		pump(2);
		if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
			test.renderer.RequestScreenshot(std::filesystem::path(qa) / "take-clip-hover.bmp");
		SDL_Event click{};
		click.type = SDL_EVENT_MOUSE_BUTTON_DOWN;
		click.button.which = TestMouse;
		click.button.windowID = motion.motion.windowID;
		click.button.button = SDL_BUTTON_LEFT;
		click.button.x = motion.motion.x;
		click.button.y = motion.motion.y;
		Require(SDL_PushEvent(&click), "Press clip Play");
		pump(1);
		click.type = SDL_EVENT_MOUSE_BUTTON_UP;
		Require(SDL_PushEvent(&click), "Release clip Play");
		pump(2);
	}
	bool checkedHold = false;
	std::optional<std::size_t> anchor;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	while (true)
	{
		pump(1);
		if (!anchor)
			for (auto index = replayFrom; index < history.Size(); ++index)
				if (history.At(index).flags & HistoryDriven)
				{
					anchor = index;
					break;
				}
		if (anchor && !checkedHold)
		{
			ImGui::GetIO().AddKeyEvent(ImGuiKey_F6, true);
			pump(1);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_F6, false);
			Eventually(
				[&] {
					pump(1);
					return test.workspace.State().timeline.playbackPaused;
				},
				"F6 did not hold the active take");
			pump(10);
			const auto held = history.Size();
			pump(10);
			Require(history.Size() == held, "Held take continued advancing frames");
			ImGui::GetIO().AddKeyEvent(ImGuiKey_F8, true);
			pump(1);
			ImGui::GetIO().AddKeyEvent(ImGuiKey_F8, false);
			checkedHold = true;
		}
		if (anchor && history.Size() >= *anchor + sourceCount &&
			!(history.At(history.Size() - 1).flags & HistoryDriven))
			break;
		if (std::chrono::steady_clock::now() >= deadline)
		{
			std::size_t driven = 0;
			for (auto index = replayFrom; index < history.Size(); ++index)
				driven += (history.At(index).flags & HistoryDriven) != 0;
			std::cout << "replay frames=" << history.Size() - replayFrom << " driven=" << driven
					  << " anchor=" << anchor.has_value() << " source=" << sourceCount << '\n';
			for (const auto& notice : test.runtime.TakeNotices())
				std::cout << "notice: " << notice.title << " " << notice.detail << '\n';
			if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
			{
				test.renderer.RequestScreenshot(std::filesystem::path(qa) / "take-failed.bmp");
				pump(1);
				test.renderer.WaitIdle();
			}
			Require(false, "Take replay did not drive and release the app");
		}
	}
	std::size_t matched = 0;
	const auto source = *history.Find(first.identity, first.epoch);
	for (std::size_t offset = 0; offset < sourceCount; ++offset)
	{
		const auto a = history.At(source + offset).hands, b = history.At(*anchor + offset).hands;
		matched += std::abs(int(a[1]) - int(b[1])) <= 2 && std::abs(int(a[0]) - int(b[0])) <= 2;
	}
	std::cout << "Replay matched " << matched << "/" << sourceCount << " frames\n";
	Require(matched == sourceCount, "Replayed input diverges from the source frames");
	const auto sampleAt = [&](std::size_t index) {
		const auto* sample = test.runtime.InputSamples().Find(history.At(index).input);
		Require(sample && sample->body && sample->trackers && sample->gaze, "Frame lacks body, gaze or trackers");
		return sample;
	};
	std::size_t devicesMatched = 0, tposed = 0;
	std::set<float> trackerPositions;
	for (std::size_t offset = 0; offset < sourceCount; ++offset)
	{
		const auto *a = sampleAt(source + offset), *b = sampleAt(*anchor + offset);
		const auto& x = (*a->trackers)[0].poseOverride;
		const auto& y = (*b->trackers)[0].poseOverride;
		tposed += a->body->preset == simxr::BodyPreset::TPose;
		if (x)
			trackerPositions.insert(x->position.x);
		const bool same =
			a->body->preset == b->body->preset && a->gaze->yaw == b->gaze->yaw && x.has_value() == y.has_value() &&
			(!x ||
			 (std::abs(x->position.x - y->position.x) < 1e-4f && std::abs(x->position.z - y->position.z) < 1e-4f));
		devicesMatched += same;
		if (!same)
			std::cout << "device mismatch at " << offset << ": preset " << int(a->body->preset) << "/"
					  << int(b->body->preset) << " yaw " << a->gaze->yaw << "/" << b->gaze->yaw << " tracker "
					  << (x ? x->position.x : -1) << "/" << (y ? y->position.x : -1) << '\n';
	}
	std::cout << "Body/gaze/tracker replay matched " << devicesMatched << "/" << sourceCount << " frames (" << tposed
			  << " in T pose, " << trackerPositions.size() << " tracker positions)\n";
	Require(tposed > 0 && tposed < sourceCount && trackerPositions.size() >= 3,
			"Source range lacks T pose or tracker motion");
	Require(devicesMatched == sourceCount, "Replayed body, gaze or trackers diverge from the source");

	std::size_t past = source;
	while (sampleAt(past)->body->preset != simxr::BodyPreset::TPose)
		++past;
	test.runtime.LookAt(past + 5);
	test.workspace.Select(DeviceSel{DevicePage::Body});
	pump(4);
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / "past-body.bmp");
		pump(1);
		test.workspace.Select(DeviceSel{DevicePage::Trackers});
		pump(3);
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / "past-trackers.bmp");
		pump(1);
		test.workspace.Select(FrameSel{history.At(past + 5).identity, history.At(past + 5).epoch});
		pump(3);
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / "past-frame.bmp");
		pump(1);
	}
	test.runtime.SetLive(true);
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / "take-replay.bmp");
		pump(1);
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors during take replay");
	std::cout << "PASS retroactive take replayed over its source frames\n";
}

void CheckLayouts(Interaction& test, const std::filesystem::path& qa)
{
	const auto pump = [&](int count) {
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto click = [&](ImVec2 point) {
		for (const bool down : {true, false})
		{
			SDL_Event event{};
			event.type = SDL_EVENT_MOUSE_MOTION;
			event.motion.windowID = SDL_GetWindowID(test.window.Native());
			event.motion.which = TestMouse;
			event.motion.x = point.x;
			event.motion.y = point.y;
			Require(SDL_PushEvent(&event), "Push layout motion");
			event = {};
			event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.windowID = SDL_GetWindowID(test.window.Native());
			event.button.which = TestMouse;
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = down;
			event.button.x = point.x;
			event.button.y = point.y;
			Require(SDL_PushEvent(&event), "Push layout click");
			pump(2);
		}
	};

	struct Size
	{
		int width, height;
		float scale;
	};

	const std::array<Selection, 3> pages{DeviceSel{DevicePage::Player}, DeviceSel{DevicePage::Right},
										 DeviceSel{DevicePage::Head}};
	for (const auto size : {Size{1280, 720, 1}, Size{1280, 720, 1.5f}, Size{1920, 1080, 1}, Size{1920, 1080, 1.5f},
							Size{1024, 640, 1.5f}})
	{
		test.window.Resize(size.width, size.height);
		test.workspace.SetScale(size.scale);
		const bool overlay = size.width / size.scale < 800;
		for (std::size_t page = 0; page < pages.size(); ++page)
		{
			test.workspace.Select(pages[page]);
			pump(6);
			const auto& shell = test.workspace.State();
			Require(shell.inspectorVisible, "Selecting a page did not show the inspector");
			for (const char* name :
				 {"Top bar", overlay ? "Device rail" : "Device tabs", "Inspector", "Timeline", "GPU preview"})
			{
				const auto* window = FindPanel(name);
				Require(window && !window->ScrollbarX, "Shell region has horizontal scrolling");
				Require(window->Pos.x >= 0 && window->Pos.x + window->Size.x <= size.width + 1,
						"Shell region extends beyond window");
			}
			Require(FindPanel("Inspector")->Pos.x + FindPanel("Inspector")->Size.x <= size.width + 1,
					"Inspector is clipped");
			const auto name = std::to_string(size.width) + "-" + std::to_string(static_cast<int>(size.scale * 100)) +
							  "-" + std::to_string(page) + ".bmp";
			test.renderer.RequestScreenshot(qa / name);
			pump(2);
			if (page == 0)
			{
				const auto rig = FindPanel("Inspector")->GetID("Rig view");
				ImGui::ActivateItemByID(ImHashStr("?##Rig help", 0, rig));
				pump(3);
				const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
				Require(!popups.empty() && popups.back().Window, "Rig help did not open");
				const auto* help = popups.back().Window;
				Require(help->Pos.x >= 0 && help->Pos.x + help->Size.x <= size.width + 1, "Rig help is clipped");
				test.renderer.RequestScreenshot(qa / ("help-" + name));
				pump(2);
				ImGui::ClosePopupsExceptModals();
				pump(2);
			}
		}
		if (overlay)
		{
			const auto& shell = test.workspace.State();
			const auto* inspector = FindPanel("Inspector");
			const auto* preview = FindPanel("GPU preview");
			Require(inspector->Pos.x < preview->Pos.x + preview->Size.x, "Overlay drawer does not cover the preview");
			Require(inspector->ParentWindow == preview->ParentWindow &&
						inspector->BeginOrderWithinParent > preview->BeginOrderWithinParent,
					"Overlay drawer is drawn under the preview");
			click({preview->Pos.x + 30, preview->Pos.y + preview->Size.y * .6f});
			pump(2);
			Require(!shell.inspectorVisible, "Clicking the preview did not close the drawer");
			test.renderer.RequestScreenshot(qa / "overlay-closed.bmp");
			pump(2);
			ImGui::ActivateItemByID(FindPanel("Device rail")->GetID("Right"));
			pump(3);
			Require(shell.inspectorVisible, "Selecting a rail row did not open the drawer");
		}
	}
	test.window.Resize(1280, 800);
	test.workspace.SetScale(1);
	pump(4);
	for (const char* eye : {"Both", "Right", "Both", "Left"})
	{
		ImGui::ActivateItemByID(FindPanel("Image controls")->GetID("View"));
		pump(2);
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Image view popover did not open");
		ImGui::ActivateItemByID(popups.back().Window->GetID(eye));
		pump(4);
		ImGui::ClosePopupsExceptModals();
		pump(2);
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors in layout capture");
	std::cout << "PASS connected layouts at four sizes and the overlay drawer\n";
}

void CheckShell(Interaction& test, const char* qa)
{
	const auto pump = [&](int count) {
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto pointer = [&](ImVec2 point, std::optional<bool> down) {
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(test.window.Native());
		event.motion.x = point.x;
		event.motion.y = point.y;
		Require(SDL_PushEvent(&event), "Push shell motion");
		if (down)
		{
			event = {};
			event.type = *down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.windowID = SDL_GetWindowID(test.window.Native());
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = *down;
			event.button.x = point.x;
			event.button.y = point.y;
			Require(SDL_PushEvent(&event), "Push shell button");
		}
		pump(2);
	};
	const auto activate = [&](const char* window, const char* id) {
		auto* panel = FindPanel(window);
		Require(panel != nullptr, (std::string("Missing ") + window).c_str());
		ImGui::ActivateItemByID(panel->GetID(id));
		pump(3);
	};
	const auto screenshot = [&](const std::string& name) {
		if (qa)
		{
			test.renderer.RequestScreenshot(std::filesystem::path(qa) / name);
			pump(2);
		}
	};
	const auto& shell = test.workspace.State();
	test.window.Resize(1280, 800);
	test.workspace.SetScale(1);
	test.workspace.Select(DeviceSel{DevicePage::Head});
	pump(6);
	for (const float scale : {1.0f, 1.5f})
		for (int width = 640; width <= 1920; width += 80)
		{
			test.window.Resize(width, 800);
			test.workspace.SetScale(scale);
			pump(4);
			const auto* preview = FindPanel("GPU preview");
			const auto* area = FindPanel("Image area");
			Require(preview && area, "Preview image missing during the width sweep");
			const auto* timeline = FindPanel("Timeline");
			Require(
				timeline && timeline->ContentSize.x <= timeline->Size.x + 1,
				("Timeline transport overflows at width " + std::to_string(width) + ", scale " + std::to_string(scale))
					.c_str());
			Require(area->ContentSize.x <= area->Size.x + 1 &&
						area->Pos.x + area->Size.x <= preview->Pos.x + preview->Size.x + 1,
					("Image overflows its region at width " + std::to_string(width)).c_str());
			if (width == 640 || width == 1040)
				screenshot("width-" + std::to_string(width) + "-" + std::to_string(int(scale * 100)) + ".bmp");
		}
	test.window.Resize(1280, 800);
	test.workspace.SetScale(1);
	pump(4);
	const float timelineHeight = FindPanel("Timeline")->Size.y;
	activate("Timeline divider tab", "##Edge");
	pump(2);
	Require(FindPanel("Timeline")->Size.y < timelineHeight * .6f, "Timeline did not fold");
	screenshot("timeline-folded.bmp");
	activate("Timeline divider tab", "##Edge");
	pump(2);
	Require(std::abs(FindPanel("Timeline")->Size.y - timelineHeight) < 1, "Timeline did not unfold");
	const float previewWidth = FindPanel("GPU preview")->Size.x;
	activate("Inspector divider tab", "##Edge");
	pump(2);
	Require(!shell.inspectorVisible && FindPanel("GPU preview")->Size.x > previewWidth + 100, "Inspector did not fold");
	activate("Device rail", "Head");
	pump(2);
	Require(shell.inspectorVisible, "Clicking the open tab did not unfold the inspector");
	activate("Timeline", "Library");
	Require(shell.library && FindPanel("Library") && FindPanel("Library")->Active, "Library did not open");
	screenshot("library-drawer.bmp");
	activate("Timeline", "Library");
	Require(!shell.library && std::holds_alternative<DeviceSel>(shell.selection) &&
				std::get<DeviceSel>(shell.selection).page == DevicePage::Head,
			"Library did not close or changed the page");
	activate("Top bar", "Focus");
	pump(2);
	Require(FindPanel("Focus exit") && FindPanel("GPU preview")->Size.x > previewWidth + 100, "Focus did not expand");
	Require(!FindPanel("Focus exit")->Rect().Overlaps(FindPanel("Image controls")->Rect()),
			"Exit focus covers the image controls");
	screenshot("focus.bmp");
	activate("Focus exit", "Exit focus");
	pump(2);
	Require(std::abs(FindPanel("GPU preview")->Size.x - previewWidth) < 1, "Exit focus did not restore the layout");
	Require(SDL_ShowWindow(test.window.Native()), "Show preview interaction window");
	SDL_RaiseWindow(test.window.Native());
	Eventually(
		[&] {
			pump(1);
			return test.window.Focused();
		},
		"Preview interaction window did not receive focus");
	const auto checkControls = [&] {
		const auto* controls = FindPanel("Image controls");
		const auto* preview = FindPanel("GPU preview");
		Require(controls && controls->Active && preview && controls->Pos.x >= preview->Pos.x &&
					controls->Pos.y >= preview->Pos.y &&
					controls->Pos.x + controls->Size.x <= preview->Pos.x + preview->Size.x + 1 &&
					controls->Pos.y + controls->Size.y <= preview->Pos.y + preview->Size.y + 1,
				"Preview corner controls escape the preview");
		Require(!controls->ScrollbarX && !controls->ScrollbarY &&
					controls->ContentSize.x + controls->WindowPadding.x * 2 <= controls->Size.x + 1 &&
					controls->ContentSize.y + controls->WindowPadding.y * 2 <= controls->Size.y + 1 &&
					controls->DC.CursorMaxPos.x <= controls->InnerClipRect.Max.x + 1 &&
					controls->DC.CursorMaxPos.y <= controls->InnerClipRect.Max.y + 1 &&
					controls->InnerClipRect.GetHeight() >= ImGui::GetFrameHeight(),
				"Preview corner clips its controls or leaves the primary row incomplete");
	};
	checkControls();
	const auto chooseView = [&](const char* choice) {
		activate("Image controls", "View");
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Image view popover did not open");
		ImGui::ActivateItemByID(popups.back().Window->GetID(choice));
		pump(3);
		ImGui::ClosePopupsExceptModals();
		pump(2);
	};
	chooseView("2:1");
	pointer(FindPanel("Image area")->InnerRect.GetCenter(), std::nullopt);
	const auto controlPoint = FindPanel("Image controls")->InnerRect.GetCenter();
	SDL_Event motion{};
	motion.type = SDL_EVENT_MOUSE_MOTION;
	motion.motion.windowID = SDL_GetWindowID(test.window.Native());
	motion.motion.which = TestMouse;
	motion.motion.x = controlPoint.x;
	motion.motion.y = controlPoint.y;
	Require(SDL_PushEvent(&motion), "Move onto preview controls in the capture frame");
	const auto rightButton = [&](bool down) {
		SDL_Event event{};
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.windowID = SDL_GetWindowID(test.window.Native());
		event.button.which = TestMouse;
		event.button.button = SDL_BUTTON_RIGHT;
		event.button.down = down;
		event.button.x = controlPoint.x;
		event.button.y = controlPoint.y;
		Require(SDL_PushEvent(&event), "Push preview control capture gesture");
	};
	rightButton(true);
	bool capturedBehindControls = false;
	for (int frame = 0; frame < 12; ++frame)
	{
		pump(1);
		capturedBehindControls |= SDL_GetWindowRelativeMouseMode(test.window.Native());
	}
	rightButton(false);
	pump(2);
	Require(!capturedBehindControls, "Moving onto preview controls started piloting behind them");
	chooseView("Fit");
	const auto pickRegion = [&] {
		activate("Image controls", "View");
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Image view popover did not open");
		ImGui::ActivateItemByID(popups.back().Window->GetID("Pick region"));
		pump(3);
	};
	const auto* area = FindPanel("Image area");
	const ImVec2 corner{area->Pos.x + area->Size.x * .3f, area->Pos.y + area->Size.y * .3f};
	const ImVec2 end{corner.x + area->Size.x * .2f, corner.y + area->Size.y * .2f};
	pickRegion();
	pointer(corner, true);
	test.runtime.FocusLost();
	pointer(end, false);
	Require(!test.runtime.PreviewCrop(), "Focus loss allowed the old crop drag to commit");
	pickRegion();
	pointer(corner, true);
	const ImVec4 replacement{.1f, .2f, .8f, .9f};
	test.runtime.PreviewCrop() = replacement;
	pointer(end, false);
	const auto unchanged = test.runtime.PreviewCrop();
	Require(unchanged && unchanged->x == replacement.x && unchanged->y == replacement.y &&
				unchanged->z == replacement.z && unchanged->w == replacement.w,
			"Changing the crop target allowed an old drag to overwrite it");
	test.runtime.PreviewCrop().reset();
	pump(3);
	pickRegion();
	pointer(corner, true);
	test.runtime.CancelPreviewInteraction();
	pointer(end, false);
	Require(!test.runtime.PreviewCrop(), "Retiring the preview gesture allowed its old drag to commit");
	pickRegion();
	pointer(corner, true);
	pointer(end, std::nullopt);
	pointer(end, false);
	pump(3);
	Require(test.runtime.PreviewCrop().has_value(), "Dragging over the image did not pin a crop");
	checkControls();
	screenshot("crop-live.bmp");
	test.window.Resize(640, 480);
	test.workspace.SetScale(2.5f);
	pump(4);
	checkControls();
	screenshot("crop-enlarged.bmp");
	test.window.Resize(1280, 800);
	test.workspace.SetScale(1);
	pump(4);
	activate("Timeline", "Pause");
	Eventually(
		[&] {
			pump(1);
			return test.runtime.Snapshot().manual && !test.runtime.Snapshot().operationPending;
		},
		"Shell check did not pause");
	for (int step = 0; step < 5; ++step)
	{
		activate("Timeline", "Step forward");
		Eventually(
			[&] {
				pump(1);
				return !test.runtime.Snapshot().operationPending;
			},
			"Step did not finish");
	}
	Require(test.runtime.PreviewCrop().has_value(), "Stepping cleared the crop");
	screenshot("crop.bmp");
	const auto edge = test.runtime.Snapshot().time.cursor.frame;
	activate("Timeline", "Step back");
	activate("Timeline", "Step back");
	const auto* back = std::get_if<FrameSel>(&shell.selection);
	Require(back && back->identity.frame < edge && !test.runtime.Snapshot().time.following,
			"Step back did not select an older frame");
	const auto older = back->identity.frame;
	activate("Timeline", "Step forward");
	const auto* forward = std::get_if<FrameSel>(&shell.selection);
	Require(forward && forward->identity.frame > older, "Step forward did not select a newer frame");
	test.runtime.PreviewCrop().reset();
	activate("Timeline", "Continue");
	Eventually(
		[&] {
			pump(1);
			return !test.runtime.Snapshot().manual && test.runtime.Snapshot().time.following;
		},
		"Shell check did not continue");
	std::size_t late = 0;
	Eventually(
		[&] {
			pump(5);
			const auto& history = test.runtime.History();
			late = 0;
			for (std::size_t index = history.Size() > 600 ? history.Size() - 600 : 0; index < history.Size(); ++index)
				late += (history.At(index).flags & HistoryLate) != 0;
			return late > 0;
		},
		"No missed frames marked from the slowed probe");
	activate("Timeline", "Slow frames");
	pump(4);
	screenshot("slow-frames.bmp");
	activate("Timeline", "Slow frames");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors in the shell check");
	std::cout << "PASS shell: image fits 640-1920 at 100/150 %, folds, focus, Library toggle, crop across steps, "
			  << late << " missed frames marked\n";
}

// D12 feedback: ImGui reports duplicate IDs only for the hovered item, so sweep the pointer
// over every page (the Frame page with its value sections open) and collect conflicts.
void CheckIdConflicts(Interaction& test, bool frameOnly)
{
	ImGui::GetIO().ConfigDebugHighlightIdConflicts = true;
	const auto pump = [&](int count) {
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(5);
		}
	};
	const auto move = [&](ImVec2 point) {
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(test.window.Native());
		event.motion.which = TestMouse;
		event.motion.x = point.x;
		event.motion.y = point.y;
		Require(SDL_PushEvent(&event), "Push sweep motion");
	};
	std::set<std::string> conflicts;
	const auto sweep = [&](const char* page, const char* region) {
		const auto* window = FindPanel(region);
		if (!window)
			return;
		const auto min = window->Rect().Min, max = window->Rect().Max;
		for (float y = min.y + 2; y < max.y; y += 10)
			for (float x = min.x + 4; x < max.x; x += 22)
			{
				move({x, y});
				test.Frame();
				const auto& g = *ImGui::GetCurrentContext();
				if (g.DebugDrawIdConflictsId)
				{
					const char* window = g.HoveredWindow ? g.HoveredWindow->Name : "?";
					conflicts.insert(std::string(page) + " in " + window + " at " + std::to_string(int(x)) + "," +
									 std::to_string(int(y)));
				}
			}
	};
	test.window.Resize(1280, 800);
	test.workspace.SetScale(1);
	const std::array<Selection, 7> pages{DeviceSel{DevicePage::Player},	 DeviceSel{DevicePage::Head},
										 DeviceSel{DevicePage::Left},	 DeviceSel{DevicePage::Right},
										 DeviceSel{DevicePage::Body},	 DeviceSel{DevicePage::Gaze},
										 DeviceSel{DevicePage::Trackers}};
	const auto openSections = [&] {
		for (const char* region : {"Inspector", "Head details", "Hand details", "Configuration details"})
			if (auto* window = FindPanel(region))
				for (const char* header : {"Position & rotation", "Role", "Connection and profile", "Output",
										   "Advanced connection", "Advanced stepping"})
					window->StateStorage.SetInt(window->GetID(header), 1);
		pump(3);
	};
	if (!frameOnly)
	{
		for (std::size_t page = 0; page < pages.size(); ++page)
		{
			test.workspace.Select(pages[page]);
			pump(4);
			openSections();
			sweep(("page " + std::to_string(page)).c_str(), "Inspector");
			for (const char* region : {"Inspector", "Head details", "Hand details", "Configuration details"})
				if (auto* details = FindPanel(region); details && details->Active && details->ScrollMax.y > 0)
				{
					ImGui::SetScrollY(details, details->ScrollMax.y);
					pump(3);
					sweep(("page bottom " + std::to_string(page)).c_str(), region);
					ImGui::SetScrollY(details, 0);
				}
		}
		const auto directory = std::filesystem::path(test.runtime.Snapshot().endpoint).parent_path();
		for (const char* folder : {"recordings", "scripts"})
		{
			if (!std::filesystem::exists(directory / folder))
				continue;
			for (const auto& entry : std::filesystem::directory_iterator(directory / folder))
			{
				if (entry.path().extension() != ".luau")
					continue;
				if (std::string_view(folder) == "recordings")
					test.workspace.Select(TakeSel{entry.path().string()});
				else
					test.workspace.Select(ScriptSel{entry.path().string()});
				pump(4);
				sweep(folder, "Inspector");
				break;
			}
		}
		ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("##App picker"));
		pump(3);
		const auto& picker = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!picker.empty() && picker.back().Window, "ID sweep app picker missing");
		sweep("app picker", picker.back().Window->Name);
		ImGui::ActivateItemByID(picker.back().Window->GetID("Edit profiles"));
		pump(4);
		Require(FindPanel("Launch profiles") && FindPanel("Launch profiles")->Active, "ID sweep profiles missing");
		sweep("launch profiles", "Launch profiles");
		if (auto* form = FindPanel("Profile form"))
		{
			ImGui::SetScrollY(form, form->ScrollMax.y);
			pump(3);
			sweep("launch profiles bottom", "Profile form");
		}
		ImGui::ClosePopupToLevel(0, true);
		pump(3);
	}
	ImGui::ActivateItemByID(FindPanel("Timeline")->GetID("Pause"));
	Eventually(
		[&] {
			pump(1);
			return test.runtime.Snapshot().manual;
		},
		"Sweep pause");
	Eventually(
		[&] {
			pump(1);
			return !test.runtime.Snapshot().operationPending;
		},
		"Sweep pause completion");
	const auto previousCapture = test.runtime.Snapshot().selectedCapture;
	ImGui::ActivateItemByID(FindPanel("Timeline")->GetID("Step forward"));
	Eventually(
		[&] {
			pump(1);
			const auto state = test.runtime.Snapshot();
			return !state.operationPending && state.selectedReady && state.selectedCapture &&
				   state.selectedCapture != previousCapture && state.inspectedFrame == state.selectedFrame;
		},
		"ID sweep exact capture not ready");
	const auto captured = test.runtime.Snapshot();
	test.workspace.Select(FrameSel{captured.selectedIdentity, captured.selectedEpoch});
	pump(30);
	auto* inspector = FindPanel("Inspector");
	for (const char* header :
		 {"All input values", "Input sources and commands", "All actions and bindings", "Layers", "Timing", "Raw"})
	{
		auto* details = FindPanel("Frame details");
		(details ? details : inspector)->StateStorage.SetInt((details ? details : inspector)->GetID(header), 1);
		pump(3);
	}
	if (auto* details = FindPanel("Frame details"))
		details->StateStorage.SetInt(details->GetID("Export image"), 1);
	pump(3);
	sweep("frame page", "Inspector");
	if (auto* details = FindPanel("Frame details"))
	{
		ImGui::SetScrollY(details, details->ScrollMax.y);
		pump(3);
		sweep("frame page bottom", "Frame details");
		if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(qa) / "id-frame-bottom.bmp");
			pump(2);
		}
		ImGui::ActivateItemByID(details->GetID("Open in window"));
		pump(4);
		Require(FindPanel("Raw metadata") && FindPanel("Raw metadata")->Active, "Raw metadata did not open");
		sweep("raw window", "Raw metadata");
	}
	ImGui::ActivateItemByID(FindPanel("Timeline")->GetID("Continue"));
	pump(4);
	for (const auto& conflict : conflicts)
		std::cout << "ID conflict: " << conflict << '\n';
	Require(conflicts.empty(), "Duplicate ImGui IDs on a page");
	std::cout << "PASS no duplicate IDs under the pointer on "
			  << (frameOnly ? "frame and raw pages" : "all checked pages") << '\n';
}

void CheckLibrary(Interaction& test, const std::filesystem::path& directory)
{
	const auto pump = [&](int count) {
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto screenshot = [&](const char* name) {
		if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(qa) / name);
			pump(2);
		}
	};
	test.workspace.Select(DeviceSel{});
	test.runtime.ResumeLive();
	Eventually(
		[&] {
			pump(1);
			return test.runtime.Snapshot().time.following && !test.runtime.Snapshot().manual;
		},
		"Library check did not start live");
	Eventually(
		[&] {
			pump(1);
			return !test.runtime.Snapshot().recordingBusy;
		},
		"Recorder did not finish its first read");
	ImGui::ActivateItemByID(FindPanel("Timeline")->GetID("Rec"));
	Eventually(
		[&] {
			pump(1);
			return test.runtime.Snapshot().recording;
		},
		"Rec did not start recording");
	DrivePilotPulses(test);
	Require(test.runtime.Snapshot().recordedChanges > 0, "Piloting recorded no input");
	ImGui::ActivateItemByID(FindPanel("Timeline divider tab")->GetID("##Edge"));
	pump(3);
	screenshot("timeline-recording-folded.bmp");
	ImGui::ActivateItemByID(FindPanel("Timeline")->GetID("Stop recording"));
	const auto popover = FindPanel("Top bar")->GetID("Recorded take");
	Eventually(
		[&] {
			pump(1);
			return ImGui::IsPopupOpen(popover, ImGuiPopupFlags_AnyPopupLevel);
		},
		"Stop did not open the save popover");
	Eventually(
		[&] {
			pump(1);
			return !test.runtime.Snapshot().recording && !test.runtime.Snapshot().recordingBusy;
		},
		"Recording did not stop");
	screenshot("library-save.bmp");
	auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
	Require(popup, "Save popover has no window");
	ImGui::ActivateItemByID(popup->GetID("Done"));
	Eventually(
		[&] {
			pump(1);
			return std::holds_alternative<TakeSel>(test.workspace.State().selection);
		},
		"Saved archive was not selected in the Library");
	const auto path = std::get<TakeSel>(test.workspace.State().selection).id;
	const auto existingPath = Utf8Path(path);
	Require(existingPath.parent_path() == directory / "recordings" && existingPath.extension() == ".simxrtake" &&
				std::filesystem::exists(existingPath / "input.simxr"),
			"Stop did not automatically persist its archive");
	auto& io = ImGui::GetIO();
	pump(3);
	screenshot("library-take.bmp");
	ImGui::ActivateItemByID(FindPanel("Timeline divider tab")->GetID("##Edge"));
	pump(3);
	ImGui::ActivateItemByID(FindPanel("Inspector")->GetID("Play take"));
	Eventually(
		[&] {
			pump(1);
			return test.runtime.Snapshot().manual;
		},
		"Library replay did not drive the app");
	pump(10);
	screenshot("library-replay.bmp");
	ImGui::ActivateItemByID(FindPanel("Inspector")->GetID("Stop take"));
	Eventually(
		[&] {
			pump(1);
			return !test.runtime.Snapshot().manual;
		},
		"Stop take did not release the app");
	std::cout << "PASS Rec, automatic archive save, Library replay and cancel\n";

	std::filesystem::create_directories(directory / "scripts");
	const auto script = directory / "scripts" / "pose_sweep.luau";
	std::ofstream(script) << R"(simxr.pause()
for sample = 0, 15 do
    local frame = simxr.frame_add(simxr.state().frame_id or "0", "1")
    local transaction = simxr.schedule({frame=frame, commands={
        {kind="take_control", channel="head"},
        {kind="head_pose", position={math.sin(sample * math.pi / 8) * 0.2, 1.7, 0}},
    }})
    while simxr.frame_before(simxr.state().frame_id or "0", frame) do
        local capture = simxr.step()
        if capture then simxr.release_capture(capture) end
        local applied = simxr.state().frame_id
        repeat
            local displayCapture, progress, source = simxr.step_display()
            if displayCapture then simxr.release_capture(displayCapture) end
        until source and not simxr.frame_before(source, applied)
    end
    local applied = simxr.await_transaction(transaction)
    simxr.checkpoint("head sample " .. sample, applied.applied_frame == frame)
end
simxr.release_inputs()
simxr.resume())";
	test.workspace.ShowLibrary(true);
	pump(3);
	ImGui::ActivateItemByID(FindPanel("Library")->GetID("Refresh library"));
	pump(3);
	screenshot("library-list.bmp");
	const auto otherTake = directory / "recordings" / "Other library take.simxrtake";
	const auto otherScript = directory / "scripts" / "other_library_script.luau";
	std::filesystem::create_directory(otherTake);
	Require(std::filesystem::copy_file(existingPath / "input.simxr", otherTake / "input.simxr") &&
				std::filesystem::copy_file(script, otherScript),
			"Could not prepare same-kind Library selections");
	int oldWidth = 0, oldHeight = 0;
	Require(SDL_GetWindowSize(test.window.Native(), &oldWidth, &oldHeight), "Could not read Library window size");
	const float oldScale = ImGui::GetStyle().FontScaleMain;
	test.workspace.SetScale(1);
	const auto reopenEntry = [&](Selection before, Selection after, const std::filesystem::path& entry) {
		test.window.Resize(1280, oldHeight);
		test.workspace.Select(std::move(before));
		pump(3);
		test.window.Resize(720, oldHeight);
		pump(4);
		Require(!test.workspace.State().inspectorVisible, "Narrowing the Library did not close the inspector");
		ImGui::ActivateItemByID(FindPanel("Library")->GetID("Refresh library"));
		pump(1);
		const auto entryPath = PathText(entry);
		Eventually(
			[&] {
				auto* browser = FindPanel("Library");
				Require(browser && browser->Active, "Library browser is not visible");
				const auto row = ImHashStr(entryPath.c_str(), 0, browser->GetID("Library entries"));
				ImGui::ActivateItemByID(ImHashStr("##Entry", 0, row));
				pump(1);
				return test.workspace.State().selection == after;
			},
			"Library entry did not become the selected page");
		pump(2);
		Require(test.workspace.State().inspectorVisible,
				"Selecting another Library entry of the same kind left the inspector closed");
	};
	reopenEntry(TakeSel{path}, TakeSel{PathText(otherTake)}, otherTake);
	reopenEntry(ScriptSel{PathText(script)}, ScriptSel{PathText(otherScript)}, otherScript);
	test.window.Resize(oldWidth, oldHeight);
	test.workspace.SetScale(oldScale);
	pump(3);
	std::cout << "PASS same-kind Library take/script selection reopens the narrow inspector\n";
	test.workspace.Select(ScriptSel{script.string()});
	pump(3);
	auto* inspector = FindPanel("Inspector");
	ImGui::ActivateItemByID(inspector->GetID("Run script"));
	Eventually(
		[&] {
			pump(1);
			return test.runtime.Snapshot().manual;
		},
		"Script did not start");
	ImGui::ActivateItemByID(inspector->GetID("Pause script"));
	pump(5);
	ImGui::ActivateItemByID(inspector->GetID("Step script"));
	pump(5);
	screenshot("library-script-paused.bmp");
	ImGui::ActivateItemByID(inspector->GetID("Resume script"));
	Eventually(
		[&] {
			pump(1);
			return !test.runtime.Snapshot().manual;
		},
		"Script did not finish and release the app");
	pump(5);
	ImGui::ActivateItemByID(inspector->GetID("Save JSON report"));
	Eventually(
		[&] {
			pump(1);
			return std::filesystem::exists(directory / "scripts" / "pose_sweep.report.json");
		},
		"Script report was not saved");
	screenshot("library-script.bmp");
	Eventually(
		[&] {
			ImGui::ActivateItemByID(inspector->GetID("Rename entry"));
			pump(1);
			return ImGui::IsPopupOpen(inspector->GetID("Rename"), ImGuiPopupFlags_AnyPopupLevel);
		},
		"Library rename popup did not open");
	auto* rename = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
	Require(rename, "Library rename popup is missing");
	ImGui::ActivateItemByID(rename->GetID("##New name"));
	ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
	pump(1);
	io.AddKeyEvent(ImGuiMod_Ctrl, true);
	io.AddKeyEvent(ImGuiKey_A, true);
	pump(1);
	io.AddKeyEvent(ImGuiKey_A, false);
	io.AddKeyEvent(ImGuiMod_Ctrl, false);
	io.AddInputCharactersUTF8("pose-étude");
	pump(1);
	ImGui::ActivateItemByID(rename->GetID("Rename"));
	pump(1);
	const auto renamed = directory / "scripts" / Utf8Path("pose-étude.luau");
	Eventually(
		[&] {
			pump(1);
			const auto selected = std::get_if<ScriptSel>(&test.workspace.State().selection);
			return selected && selected->id == PathText(renamed);
		},
		"Library selection did not follow the completed rename");
	Require(std::filesystem::exists(renamed) && !std::filesystem::exists(script),
			"Library rename did not change the file");
	screenshot("library-renamed.bmp");
	Eventually(
		[&] {
			ImGui::ActivateItemByID(inspector->GetID("Delete entry"));
			pump(1);
			return ImGui::IsPopupOpen(inspector->GetID("Delete?"), ImGuiPopupFlags_AnyPopupLevel);
		},
		"Library delete popup did not open");
	auto* remove = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
	Require(remove, "Library delete popup is missing");
	ImGui::ActivateItemByID(remove->GetID("Delete"));
	pump(1);
	Eventually(
		[&] {
			pump(1);
			return !std::filesystem::exists(renamed) &&
				   std::holds_alternative<DeviceSel>(test.workspace.State().selection);
		},
		"Library deletion did not retire the file and return to the device page");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors in the Library flow");
	std::cout << "PASS Library script run, pause, step, resume, report, Unicode rename and delete\n";
}

void CheckInputPad(Interaction& test)
{
	IgnoreRealPointer();
	const auto target = test.runtime.Snapshot();
	ManualInputController observer;
	observer.SetTarget({target.endpoint, target.session, target.generation});
	const auto pump = [&](int count) {
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto observed = [&] {
		const auto before = observer.Snapshot().observation;
		observer.Refresh();
		Eventually(
			[&] {
				pump(1);
				return observer.Snapshot().observation > before;
			},
			"Pad observation stalled");
		return ReadObservedSetup(observer.Snapshot().state).values.input.controllers[1];
	};
	ImVec2 pointerPosition;
	const auto settle = [&](const auto& ready, const char* failure) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		while (true)
		{
			const auto controls = observed();
			if (ready(controls))
				return;
			if (std::chrono::steady_clock::now() >= deadline)
			{
				const auto& g = *ImGui::GetCurrentContext();
				const auto& io = ImGui::GetIO();
				std::cout << "Input pad failure: " << failure << " focus=" << test.window.Focused()
						  << " keyboardFocus=" << (SDL_GetKeyboardFocus() == test.window.Native())
						  << " mouseFocus=" << (SDL_GetMouseFocus() == test.window.Native())
						  << " appFocusLost=" << io.AppFocusLost << " requested=" << pointerPosition.x << ','
						  << pointerPosition.y << " mouse=" << io.MousePos.x << ',' << io.MousePos.y
						  << " leftDown=" << io.MouseDown[ImGuiMouseButton_Left]
						  << " rightDown=" << io.MouseDown[ImGuiMouseButton_Right] << " active=" << g.ActiveId
						  << " activeButton=" << static_cast<int>(g.ActiveIdMouseButton)
						  << " hovered=" << (g.HoveredWindow ? g.HoveredWindow->Name : "none")
						  << " trigger=" << controls.trigger << " grip=" << controls.grip << '\n';
				if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
				{
					test.renderer.RequestScreenshot(std::filesystem::path(qa) / "input-pad-failure.bmp");
					pump(2);
					test.renderer.WaitIdle();
				}
			}
			Require(std::chrono::steady_clock::now() < deadline, failure);
		}
	};
	const auto window = SDL_GetWindowID(test.window.Native());
	const auto move = [&](ImVec2 at) {
		pointerPosition = at;
		SDL_WarpMouseInWindow(test.window.Native(), at.x, at.y);
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = window;
		event.motion.which = TestMouse;
		event.motion.x = at.x;
		event.motion.y = at.y;
		Require(SDL_PushEvent(&event), "Push pad motion");
		pump(1);
	};
	const auto button = [&](unsigned code, bool down) {
		SDL_Event event{};
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.windowID = window;
		event.button.which = TestMouse;
		event.button.button = code;
		event.button.down = down;
		event.button.x = pointerPosition.x;
		event.button.y = pointerPosition.y;
		Require(SDL_PushEvent(&event), "Push pad button");
		pump(2);
	};
	const auto rect = [&](const char* name) {
		auto& g = *ImGui::GetCurrentContext();
		for (auto* candidate : g.Windows)
			if (std::string_view(candidate->Name).find("Inspector") != std::string_view::npos && candidate->Active)
			{
				const auto id = ImHashStr(name, 0, candidate->GetID(1));
				ImGui::SetFocusID(id, candidate);
				pump(1);
				if (g.NavId == id && g.NavIdIsAlive)
				{
					const auto value = ImGui::WindowRectRelToAbs(candidate, candidate->NavRectRel[0]);
					ImGui::ClearActiveID();
					g.NavId = 0;
					return value;
				}
			}
		if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot(std::filesystem::path(qa) / "input-pad-missing.bmp");
			pump(1);
			test.renderer.WaitIdle();
		}
		throw std::runtime_error(std::string("Input pad item not found: ") + name);
	};
	Require(SDL_ShowWindow(test.window.Native()), "Show pad window");
	Require(SDL_RaiseWindow(test.window.Native()), "Raise pad window");
	int width = 0, height = 0;
	Require(SDL_GetWindowSize(test.window.Native(), &width, &height), "Read pad window size");
	SDL_WarpMouseInWindow(test.window.Native(), width / 2.0f, height / 2.0f);
	test.workspace.Select(DeviceSel{DevicePage::Right});
	Eventually(
		[&] {
			pump(1);
			return test.window.Focused();
		},
		"Pad window did not receive native focus");
	pump(4);
	const auto trigger = rect("Trigger"), grip = rect("Grip"), primary = rect("Primary"), secondary = rect("Secondary"),
			   menu = rect("Menu"), stick = rect("Stick");

	move(trigger.GetCenter());
	button(SDL_BUTTON_LEFT, true);
	settle([](const auto& c) { return c.trigger > .99f; }, "Trigger hold did not press fully");
	button(SDL_BUTTON_LEFT, false);
	settle([](const auto& c) { return c.trigger == 0; }, "Trigger release did not return to zero");
	move({trigger.Min.x + 2, trigger.GetCenter().y});
	button(SDL_BUTTON_LEFT, true);
	move({trigger.Min.x + trigger.GetWidth() * .3f, trigger.GetCenter().y});
	move({trigger.Min.x + trigger.GetWidth() * .5f, trigger.GetCenter().y});
	settle([](const auto& c) { return std::abs(c.trigger - .5f) < .1f; }, "Trigger drag did not set half");
	button(SDL_BUTTON_LEFT, false);
	move({trigger.Min.x + trigger.GetWidth() * .7f, trigger.GetCenter().y});
	button(SDL_BUTTON_RIGHT, true);
	button(SDL_BUTTON_RIGHT, false);
	settle([](const auto& c) { return std::abs(c.trigger - .7f) < .1f; }, "Trigger latch did not hold 70%");
	button(SDL_BUTTON_RIGHT, true);
	button(SDL_BUTTON_RIGHT, false);
	settle([](const auto& c) { return c.trigger == 0; }, "Trigger unlatch did not release");
	std::cout << "PASS pad trigger hold, drag, latch\n";
	move(trigger.GetCenter());
	button(SDL_BUTTON_LEFT, true);
	settle([](const auto& c) { return c.trigger > .99f; }, "Hidden-pad fixture did not hold the trigger");
	test.workspace.SetPreviewExpanded(true);
	settle([](const auto& c) { return c.trigger == 0; }, "Hiding the input pad left a momentary trigger held");
	button(SDL_BUTTON_LEFT, false);
	test.workspace.SetPreviewExpanded(false);
	pump(3);
	std::cout << "PASS hidden input-pad momentary release\n";

	move(grip.GetCenter());
	button(SDL_BUTTON_LEFT, true);
	settle([](const auto& c) { return c.grip > .99f; }, "Grip hold did not press");
	button(SDL_BUTTON_LEFT, false);
	settle([](const auto& c) { return c.grip == 0; }, "Grip did not release");
	move(primary.GetCenter());
	button(SDL_BUTTON_LEFT, true);
	settle([](const auto& c) { return c.primary; }, "Primary hold did not press");
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / "input-pad-hold.bmp");
		pump(1);
	}
	button(SDL_BUTTON_LEFT, false);
	settle([](const auto& c) { return !c.primary; }, "Primary did not release");
	move(secondary.GetCenter());
	button(SDL_BUTTON_RIGHT, true);
	button(SDL_BUTTON_RIGHT, false);
	settle([](const auto& c) { return c.secondary; }, "Secondary latch did not press");
	button(SDL_BUTTON_RIGHT, true);
	button(SDL_BUTTON_RIGHT, false);
	settle([](const auto& c) { return !c.secondary; }, "Secondary unlatch did not release");
	move(menu.GetCenter());
	button(SDL_BUTTON_LEFT, true);
	settle([](const auto& c) { return c.menu; }, "Menu hold did not press");
	button(SDL_BUTTON_LEFT, false);
	settle([](const auto& c) { return !c.menu; }, "Menu did not release");
	std::cout << "PASS pad grip, face buttons and menu\n";

	move({stick.Min.x + stick.GetWidth() * .12f, stick.GetCenter().y});
	button(SDL_BUTTON_LEFT, true);
	settle([](const auto& c) { return c.stick.x < -.99f && c.stick.y == 0 && !c.thumbClick; },
		   "Stick left arrow did not push fully left");
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / "input-pad-arrow.bmp");
		pump(1);
	}
	button(SDL_BUTTON_LEFT, false);
	settle([](const auto& c) { return c.stick.x == 0 && c.stick.y == 0; }, "Stick arrow did not release");
	move(stick.GetCenter());
	button(SDL_BUTTON_LEFT, true);
	settle([](const auto& c) { return c.thumbClick; }, "Stick click did not press");
	move({stick.GetCenter().x + stick.GetWidth() * .2f, stick.GetCenter().y});
	move({stick.Max.x, stick.GetCenter().y});
	settle([](const auto& c) { return c.stick.x > .95f && !c.thumbClick; }, "Stick drag did not reach full right");
	button(SDL_BUTTON_LEFT, false);
	settle([](const auto& c) { return c.stick.x == 0 && c.stick.y == 0; }, "Stick did not spring back");
	move({stick.GetCenter().x, stick.Min.y + 2});
	button(SDL_BUTTON_RIGHT, true);
	button(SDL_BUTTON_RIGHT, false);
	move(stick.GetCenter());
	button(SDL_BUTTON_LEFT, true);
	move({stick.GetCenter().x, stick.GetCenter().y - stick.GetHeight() * .2f});
	move({stick.GetCenter().x, stick.Min.y});
	button(SDL_BUTTON_LEFT, false);
	settle([](const auto& c) { return c.stick.y > .95f; }, "Latched stick did not stay up");
	button(SDL_BUTTON_RIGHT, true);
	button(SDL_BUTTON_RIGHT, false);
	settle([](const auto& c) { return c.stick.y == 0; }, "Stick unlatch did not centre");
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / "input-pad.bmp");
		pump(1);
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors on the input pad");
	SDL_SetEventFilter(nullptr, nullptr);
	std::cout << "PASS pad stick drag, click, spring-back and latch\n";
}

void CapturePolish(Interaction& test, const std::filesystem::path& qa)
{
	const auto pump = [&](int count) {
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto screenshot = [&](const char* name) {
		test.renderer.RequestScreenshot(qa / name);
		pump(2);
	};
	const auto activate = [&](const char* window, const char* label) {
		auto* target = FindPanel(window);
		Require(target != nullptr, (std::string("Window missing: ") + window).c_str());
		ImGui::ActivateItemByID(target->GetID(label));
		pump(3);
	};
	test.window.Resize(1600, 900);
	test.workspace.SetScale(1);
	test.workspace.Select(DeviceSel{DevicePage::Right});
	pump(8);
	screenshot("polish-hand.bmp");
	const auto rig = FindPanel("Inspector")->GetID("Rig view");
	ChooseRigView(rig, "Eye", pump);
	pump(2);
	screenshot("polish-eye.bmp");
	ChooseRigView(rig, "Back", pump);
	const auto inspector = FindPanel("Inspector");
	ImGui::SetScrollY(inspector, inspector->ScrollMax.y);
	pump(2);
	ImGui::ActivateItemByID(inspector->GetID("Position & rotation"));
	pump(2);
	ImGui::SetScrollY(inspector, inspector->ScrollMax.y);
	pump(2);
	const auto rotationForm = [&](const char* field, const char* label) {
		const auto id = inspector->GetID(field);
		ImGui::SetFocusID(id, inspector);
		pump(1);
		auto& context = *ImGui::GetCurrentContext();
		Require(context.NavId == id && context.NavIdIsAlive, "Rotation field not laid out");
		const auto center = ImGui::WindowRectRelToAbs(inspector, inspector->NavRectRel[0]).GetCenter();
		ImGui::ClearActiveID();
		context.NavId = 0;
		auto& io = ImGui::GetIO();
		io.AddMousePosEvent(center.x, center.y);
		pump(1);
		io.AddMouseButtonEvent(ImGuiMouseButton_Right, true);
		pump(1);
		io.AddMouseButtonEvent(ImGuiMouseButton_Right, false);
		pump(2);
		const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
		Require(!popups.empty() && popups.back().Window, "Rotation display menu did not open");
		screenshot("polish-rotation-menu.bmp");
		ImGui::ActivateItemByID(popups.back().Window->GetID(label));
		pump(2);
	};
	rotationForm("##Yaw", "Quaternion");
	screenshot("polish-quaternion.bmp");
	rotationForm("##Qx", "Euler angles");
	const auto before = test.runtime.Changes().size();
	ImGui::ActivateItemByID(ImHashStr("Forward", 0, inspector->GetID("Hand presets")));
	Eventually(
		[&] {
			pump(1);
			return test.runtime.Changes().size() == before + 1;
		},
		"Hand preset left no history");
	Require(test.runtime.Changes().back().label == "Right hand: Forward",
			("History named the hand preset " + test.runtime.Changes().back().label).c_str());
	activate("Top bar", "History");
	const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
	Require(!popups.empty() && popups.back().Window, "History did not open");
	screenshot("polish-history.bmp");
	ImGui::ActivateItemByID(ImHashStr(test.runtime.Changes().back().label.c_str(), 0,
									  popups.back().Window->GetID(static_cast<int>(before))));
	Eventually(
		[&] {
			pump(1);
			return test.runtime.Changes().size() == before;
		},
		"History did not undo the change");
	test.workspace.Select(DeviceSel{DevicePage::Player});
	pump(2);
	ImGui::SetScrollY(FindPanel("Inspector"), 0);
	pump(2);
	screenshot("polish-player.bmp");
	test.workspace.Select(DeviceSel{DevicePage::Right});
	pump(2);
	activate("HUD hand", "Fold HUD");
	Require(FindPanel("HUD strip") != nullptr, "Folded HUD shows no way back");
	screenshot("polish-hud-folded.bmp");
	activate("HUD strip", "Show HUD");
	activate("Timeline", "Rec");
	Eventually(
		[&] {
			pump(1);
			return test.runtime.Snapshot().recording;
		},
		"Rec did not start recording");
	pump(90);
	screenshot("polish-recording.bmp");
	activate("Timeline", "Pause");
	Eventually(
		[&] {
			pump(1);
			return test.runtime.Snapshot().manual;
		},
		"Pause did not pause");
	screenshot("polish-paused.bmp");
	activate("Inspector divider tab", "##Edge");
	activate("Timeline divider tab", "##Edge");
	Require(!test.workspace.State().inspectorVisible, "Inspector divider tab did not fold the inspector");
	screenshot("polish-folded.bmp");
	activate("Inspector divider tab", "##Edge");
	activate("Timeline divider tab", "##Edge");
	Require(test.workspace.State().inspectorVisible, "Inspector divider tab did not unfold the inspector");
	activate("Timeline", "Continue");
	activate("Top bar", "Focus");
	screenshot("polish-focus-recording.bmp");
	activate("Top bar", "Focus");
	activate("Timeline", "Stop recording");
	Eventually(
		[&] {
			pump(1);
			return !test.runtime.Snapshot().recording;
		},
		"Stop recording did not stop");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors during the polish capture");
	std::cout << "PASS polish capture\n";
}

void CheckMeasuredTiming(Interaction& test)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(6);
	while (std::chrono::steady_clock::now() < deadline)
		test.Frame();
	const auto& history = test.runtime.History();
	std::size_t cpu = 0, gpu = 0, slow = 0, fast = 0;
	for (std::size_t index = 0; index < history.Size(); ++index)
	{
		const auto& frame = history.At(index);
		cpu += frame.cpuMilliseconds >= 0;
		gpu += frame.compositorGpuMilliseconds >= 0;
		slow += frame.cpuMilliseconds >= 35;
		fast += frame.cpuMilliseconds >= 0 && frame.cpuMilliseconds < 20;
	}
	Require(cpu > 50 && gpu > 20 && slow > 2 && fast > 20,
			"Measured CPU/GPU timing did not distinguish the deliberately slow probe frames");
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot(std::filesystem::path(qa) / "measured-timing.bmp");
		test.Frame();
	}
	test.renderer.WaitIdle();
	Require(!test.validation->errors.load(), "GPU timing introduced validation errors");
	std::cout << "PASS measured timing: CPU " << cpu << ", compositor GPU " << gpu << ", slow " << slow << ", fast "
			  << fast << '\n';
}

#endif
}
