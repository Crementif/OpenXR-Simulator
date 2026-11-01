#include "app/UiPch.h"
#include "Cases.h"
#include "app/services/PlayerRig.h"
#include "app/services/ManualInputController.h"
#include "app/services/SimulationEdits.h"
#include "app/services/SpatialMath.h"
#include "app/services/RuntimeConnection.h"
#include "app/widgets/PilotController.h"
#include "app/pages/DeviceControlPanel.h"
#include "app/services/Utf8Path.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include <iostream>
#include <random>
#ifdef __linux__
#include <unistd.h>
#endif
#ifdef _WIN32
#include <windows.h>
#endif

namespace simxr::tests
{
void CheckDebuggerWorkflow(const char* endpoint)
{
	const auto directory = QaDirectory("SIMXR_DEBUG_QA_DIRECTORY");
	Require(!directory.empty(), "Debugger QA requires a new private output directory");
	Require(directory.is_absolute() && std::filesystem::create_directory(directory), "Debugger output must be new");
#ifdef _WIN32
	Require(SetEnvironmentVariableW(L"SIMXR_COMPANION_SETTINGS", (directory / "companion.settings").c_str()),
			"Isolate debugger settings");
#else
	Require(::setenv("SIMXR_COMPANION_SETTINGS", (directory / "companion.settings").c_str(), 1) == 0,
			"Isolate debugger settings");
#endif
	Interaction test(true);
	test.runtime.ConnectEndpoint(endpoint);
	std::ofstream notices(directory / "notices.log", std::ios::binary);
	Require(notices.good(), "Open debugger notice log");
	std::string lastNotice;
	std::uint64_t composedCapture = 0;
	const auto diagnose = [&](const RuntimeView& state, const char* failure) {
		namespace wire = simxr::control;
		namespace tag = wire::tag;
		std::ofstream output(directory / "timeout.txt", std::ios::binary);
		Require(output.good(), "Open debugger timeout diagnostics");
		output << failure << "\nsession=" << state.session << " generation=" << state.generation
			   << " operation=" << state.lastOperation << " selected_capture=" << state.selectedCapture
			   << " last_capture=" << state.lastCapture << " image_capture=" << state.selectedImageCapture
			   << " selected_frame=" << state.selectedFrame << " ready=" << state.selectedReady
			   << " slices=" << state.selectedLayers << "\nerror=" << state.error << "\nphase=" << state.phase
			   << "\nprogress=" << state.progress << "\nnotice=" << state.notice << '\n';
		output.flush();
		try
		{
			RuntimeConnection observer;
			observer.Connect(endpoint);
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
			Eventually(
				[&] {
					const auto connection = observer.Snapshot();
					if (connection.state == RuntimeConnectionState::Failed)
						throw std::runtime_error(connection.detail);
					return connection.state == RuntimeConnectionState::Connected;
				},
				"Timeout observer connection failed");
			const auto query = [&](const std::string& name, wire::OpCode opcode, std::vector<wire::Field> fields) {
				Require(std::chrono::steady_clock::now() < deadline, "Timeout diagnostic deadline exceeded");
				wire::ControlPacket request;
				request.message.opcode = static_cast<std::uint16_t>(opcode);
				request.message.fields = std::move(fields);
				const auto ticket = observer.Submit(std::move(request));
				Require(ticket != 0, "Timeout observer request was not admitted");
				while (true)
				{
					for (auto& reply : observer.Poll())
						if (reply.ticket == ticket)
						{
							if (!reply.error.empty())
								throw std::runtime_error(name + ": " + reply.error);
							auto& message = reply.packet.message;
							const auto bytes = wire::Encode(message);
							std::ofstream binary(directory / ("timeout-" + name + ".wire"), std::ios::binary);
							binary.write(reinterpret_cast<const char*>(bytes.data()),
										 static_cast<std::streamsize>(bytes.size()));
							binary.close();
							Require(binary.good(), "Write bounded timeout protocol response");
							const auto target = panel::Nested(message.Find(tag::Target));
							output << name << ": error=" << panel::U32(message, tag::Error)
								   << " detail=" << panel::Text(message, tag::Detail)
								   << " phase=" << panel::Text(message, tag::Phase)
								   << " progress=" << panel::Text(message, tag::Progress);
							if (const auto* result = message.Find(tag::Result); result && result->AsI64())
								output << " result=" << *result->AsI64();
							output << " capture=" << panel::U64(message, tag::CaptureId)
								   << " frame=" << panel::Frame(message)
								   << " image_available=" << panel::Boolean(message, tag::ImageAvailable)
								   << " target=" << panel::U32(target, tag::CaptureKind)
								   << " layer=" << panel::U32(target, tag::Layer)
								   << " view=" << panel::U32(target, tag::View) << '\n';
							if (const auto* pipeline = message.Find(tag::Pipeline))
							{
								const auto value = panel::Nested(pipeline);
								output << "pipeline: running=" << panel::Boolean(value, tag::Running)
									   << " paused=" << panel::Boolean(value, tag::Paused)
									   << " wait_pending=" << panel::Boolean(value, tag::WaitPending)
									   << " permits=" << panel::U32(value, tag::Permits)
									   << " next_admission=" << panel::U64(value, tag::NextAdmission)
									   << " waited=" << panel::U64(panel::Nested(value.Find(tag::Waited)), tag::FrameId)
									   << " begun=" << panel::U64(panel::Nested(value.Find(tag::Begun)), tag::FrameId)
									   << " submitted="
									   << panel::U64(panel::Nested(message.Find(tag::Presentation)), tag::LastSubmitted)
									   << '\n';
							}
							output.flush();
							Require(output.good(), "Write debugger timeout summary");
							return std::move(message);
						}
					Require(std::chrono::steady_clock::now() < deadline, "Timeout diagnostic deadline exceeded");
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				}
			};
			const auto sessionFields = [&] {
				return std::vector{wire::Field::U64(tag::Session, state.session, true),
								   wire::Field::U64(tag::Generation, state.generation, true)};
			};
			std::vector<std::uint64_t> captures;
			const auto addCapture = [&](std::uint64_t identity) {
				if (identity && captures.size() < 4 &&
					std::find(captures.begin(), captures.end(), identity) == captures.end())
					captures.push_back(identity);
			};
			if (state.session)
				query("status", wire::OpCode::Status, sessionFields());
			else
				query("sessions", wire::OpCode::Sessions, {});
			if (state.lastOperation)
			{
				auto fields = sessionFields();
				fields.push_back(wire::Field::U64(tag::OperationId, state.lastOperation, true));
				const auto operation = query("operation", wire::OpCode::Operation, std::move(fields));
				addCapture(panel::U64(operation, tag::CaptureId));
				for (const auto& field : panel::Nested(operation.Find(tag::Captures)).fields)
					addCapture(field.AsU64().value_or(0));
			}
			addCapture(state.lastCapture);
			addCapture(state.selectedCapture);
			addCapture(composedCapture);
			addCapture(state.selectedImageCapture);
			for (const auto capture : captures)
			{
				auto fields = sessionFields();
				fields.push_back(wire::Field::U64(tag::CaptureId, capture, true));
				query("capture-" + std::to_string(capture), wire::OpCode::Capture, fields);
				const auto inspection =
					query("inspect-" + std::to_string(capture), wire::OpCode::InspectFrame, std::move(fields));
				if (const auto identity = panel::U64(inspection, tag::InspectionId))
					query("release-inspection-" + std::to_string(identity), wire::OpCode::ReleaseInspection,
						  {wire::Field::U64(tag::InspectionId, identity, true)});
			}
			observer.Close();
		}
		catch (const std::exception& error)
		{
			output << "diagnostic_error=" << error.what() << '\n';
		}
		output.close();
		Require(output.good(), "Close debugger timeout diagnostics");
	};
	const auto wait = [&](const auto& ready, const char* failure) {
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		do
		{
			test.Frame();
			const auto observed = test.runtime.Snapshot();
			if (!observed.notice.empty() && observed.notice != lastNotice)
			{
				notices << "frame=" << observed.selectedFrame << ' ' << observed.notice << '\n';
				notices.flush();
				Require(notices.good(), "Write debugger notice detail");
				lastNotice = observed.notice;
			}
			if (std::chrono::steady_clock::now() >= deadline)
			{
				const auto state = test.runtime.Snapshot();
				try
				{
					diagnose(state, failure);
				}
				catch (const std::exception& error)
				{
					std::cerr << "Timeout diagnostics failed: " << error.what() << '\n';
				}
				throw std::runtime_error(std::string(failure) + ": " + state.phase + "/" + state.progress + " " +
										 state.error);
			}
		}
		while (!ready(test.runtime.Snapshot()));
	};
	wait([](const auto& state) { return state.selectedReady; }, "Debugger did not attach live output");
	test.SelectWorkspace(FrameSel{});
	test.runtime.Command(simxr::control::CommandKind::Pause);
	wait([](const auto& state) { return state.manual && !state.operationPending && state.phase == "complete"; },
		 "Pause failed");
	test.runtime.Command(simxr::control::CommandKind::StrictAdmission);
	wait(
		[](const auto& state) {
			return !state.operationPending && state.progress == "issued" && state.selectedReady &&
				   state.selectedImageCapture == state.selectedCapture;
		},
		"One admitted frame did not produce a retained image");
	const auto exact = test.runtime.Snapshot();
	composedCapture = exact.selectedCapture;
	test.runtime.InspectSelected();
	wait([&](const auto& state) { return state.inspectedFrame == exact.selectedFrame; },
		 "Exact state inspection failed");
	Require(test.runtime.RequestExport(PathText(directory / "composed.png")), "Composed export was not accepted");
	wait([&](const auto&) { return !test.runtime.ImageExport().Busy(); }, "Composed export failed");
	Require(test.runtime.ImageExport().Succeeded(), test.runtime.ImageExport().Detail().c_str());
	Require(std::filesystem::file_size(directory / "composed.png") > 100, "Composed export is empty");
	test.renderer.RequestScreenshot(directory / "inspected.bmp");
	test.Frame();
	test.runtime.Command(simxr::control::CommandKind::StrictAdmission, 0, true, 0, 0);
	wait(
		[&](const auto& state) {
			return !state.operationPending && state.selectedFrame > exact.selectedFrame && state.selectedReady &&
				   state.selectedLayers == 1 && state.selectedImageCapture == state.selectedCapture;
		},
		"Selected layer view did not produce a retained image");
	const auto raw = test.runtime.Snapshot();
	Require(raw.selectedFrame == exact.selectedFrame + 1, "Strict step admitted more than one frame");
	Require(test.runtime.RequestExport(PathText(directory / "layer.png")), "Layer export was not accepted");
	wait([&](const auto&) { return !test.runtime.ImageExport().Busy(); }, "Layer export failed");
	Require(test.runtime.ImageExport().Succeeded(), test.runtime.ImageExport().Detail().c_str());
	Require(std::filesystem::file_size(directory / "layer.png") > 100, "Layer export is empty");
	test.workspace.Select(DeviceSel{DevicePage::Head});
	test.Frame();
	test.runtime.Step();
	wait(
		[&](const auto& state) {
			const auto* selected = std::get_if<FrameSel>(&test.workspace.State().selection);
			return !state.operationPending && state.selectedReady && state.selectedFrame && selected &&
				   selected->identity == state.selectedIdentity && selected->epoch == state.selectedEpoch;
		},
		"Requested single-frame step did not open its Frame page");
	std::cout << "PASS requested single-frame step opens the captured Frame page from a device page\n";
	for (const auto capture : test.runtime.CaptureIds())
		test.runtime.Command(simxr::control::CommandKind::Release, capture);
	wait([](const auto& state) { return !state.operationPending; }, "Capture release failed");
	test.runtime.Command(simxr::control::CommandKind::Resume);
	test.runtime.SetLive(true);
	wait(
		[&](const auto& state) {
			return !state.operationPending && !state.manual && state.selectedFrame > raw.selectedFrame;
		},
		"Resume did not restore advancing live output");
	test.renderer.RequestScreenshot(directory / "resumed.bmp");
	test.Frame();
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Debugger workflow reported Vulkan validation errors");
	std::ofstream(directory / "frames.txt")
		<< "composed=" << exact.selectedFrame << " layer=" << raw.selectedFrame << " inspected=" << exact.selectedFrame
		<< " resumed=" << test.runtime.Snapshot().selectedFrame << '\n';
	std::cout << "PASS production pause/strict-step/inspect/composed-and-layer-export/resume: frame "
			  << exact.selectedFrame << " -> " << raw.selectedFrame << '\n';
	const auto previousEpoch = test.runtime.Snapshot().epoch;
	test.runtime.Disconnect();
	test.Frame();
	const auto disconnected = test.runtime.Snapshot();
	Require(!disconnected.selectedReady && disconnected.selectedFrame == 0 &&
				disconnected.selectedIdentity.frame == 0 && disconnected.time.following,
			"Disconnect left the old runtime's preview selected");
	test.runtime.ConnectEndpoint(endpoint);
	wait(
		[&](const auto& state) {
			return state.connected && state.epoch != previousEpoch && state.selectedReady && state.selectedFrame;
		},
		"Reconnect did not select a fresh runtime preview");
	Require(test.validation->errors.load() == 0, "Runtime reconnect reported Vulkan validation errors");
	notices.close();
	Require(notices.good(), "Close debugger notice log");
	std::cout << "PASS disconnect clears selected image and reconnect selects the new connection epoch\n";
}

#ifdef __linux__
void CheckManualEdges(const RuntimeView& target)
{
	RecordingController recording;
	recording.SetTarget({target.endpoint, target.session, target.generation});
	Require(recording.Request(RecordingAction::Start), "Start edge recording");
	Eventually(
		[&] {
			const auto s = recording.Snapshot();
			Require(s.error.empty(), s.error.c_str());
			return s.page && s.page->active && !s.busy;
		},
		"Edge recording did not start");
	ManualInputController input;
	input.SetTarget({target.endpoint, target.session, target.generation});
	Eventually([&] { return input.Snapshot().observation != 0; }, "Edge worker did not connect");
	simxr::ControllerControls controls;
	controls.primary = true;
	ManualTransaction press;
	press.commands = {simxr::TakeControl{simxr::ControlChannel::RightControls, false},
					  simxr::SetControllerControls{1, controls}};
	Require(input.Submit(press), "Press was refused");
	std::this_thread::sleep_for(std::chrono::milliseconds(30));
	controls.primary = false;
	ManualTransaction release;
	release.releaseEdge = true;
	release.commands = {simxr::TakeControl{simxr::ControlChannel::RightControls, false},
						simxr::SetControllerControls{1, controls}};
	Require(input.Submit(release), "Release was refused");
	Eventually(
		[&] {
			const auto s = input.Snapshot();
			Require(s.error.empty(), s.error.c_str());
			return !s.editPending;
		},
		"Button edges did not apply");
	Require(!ReadObservedSetup(input.Snapshot().state).values.input.controllers[1].primary, "Primary remained held");
	Require(recording.Request(RecordingAction::Stop), "Stop edge recording");
	Eventually(
		[&] {
			const auto s = recording.Snapshot();
			Require(s.error.empty(), s.error.c_str());
			return s.page && !s.page->active && !s.busy;
		},
		"Edge recording did not stop");
	Require(recording.Request(RecordingAction::Read), "Read recorded edges");
	Eventually(
		[&] {
			const auto s = recording.Snapshot();
			Require(s.error.empty(), s.error.c_str());
			return s.page && !s.busy;
		},
		"Edge recording read did not finish");
	std::uint64_t pressFrame = 0, releaseFrame = 0;
	for (const auto& transaction : recording.Snapshot().page->transactions)
		for (const auto& command : transaction.commands)
			if (const auto value = std::get_if<simxr::SetControllerControls>(&command); value && value->hand == 1)
			{
				Require(transaction.phase == "applied", "Recorded edge was not applied");
				if (value->value.primary)
					pressFrame = transaction.frame.value;
				else if (pressFrame)
					releaseFrame = transaction.frame.value;
			}
	Require(pressFrame && releaseFrame > pressFrame, "30 ms tap was not applied at distinct frames");
	Require(input.Submit(ReleaseManualInputs{}), "Final release refused");
	Eventually([&] { return !input.Snapshot().editPending; }, "Final release did not complete");
	ManualInputController rival;
	rival.SetTarget({target.endpoint, target.session, target.generation});
	Eventually([&] { return rival.Snapshot().observation != 0; }, "Rival worker did not connect");
	ManualTransaction headClaim;
	headClaim.commands = {simxr::TakeControl{simxr::ControlChannel::Head, false}};
	Require(rival.Submit(headClaim), "Rival claim refused");
	Eventually([&] { return !rival.Snapshot().editPending; }, "Rival claim did not apply");
	Require(input.Submit(press), "Held press refused");
	Eventually([&] { return !input.Snapshot().editPending; }, "Held press did not apply");
	Require(input.Submit(headClaim), "Conflict submission refused");
	Eventually([&] { return !input.Snapshot().error.empty(); }, "Conflict did not reject");
	Eventually([&] { return !ReadObservedSetup(input.Snapshot().state).values.input.controllers[1].primary; },
			   "Rejected transaction stranded a held primary button");
	Require(rival.Submit(ReleaseManualInputs{}), "Rival release refused");
	Eventually([&] { return !rival.Snapshot().editPending; }, "Rival release did not complete");
	std::cout << "PASS native rejection retires held inputs\n";
	std::cout << "PASS native 30 ms tap: press frame " << pressFrame << ", release frame " << releaseFrame << "\n";
}

void CheckNativePlayerRig(const RuntimeView& target)
{
	ManualInputController input;
	input.SetTarget({target.endpoint, target.session, target.generation});
	Eventually([&] { return input.Snapshot().observation != 0; }, "Rig worker did not connect");
	PlayerRig rig;
	rig.Reset(ReadObservedSetup(input.Snapshot().state));
	const auto submit = [&] {
		const auto before = input.Snapshot();
		Require(input.Submit(rig.Transaction(ReadObservedSetup(before.state), before.source)), "Rig edit refused");
		Eventually(
			[&] {
				const auto state = input.Snapshot();
				Require(state.error.empty(), state.error.c_str());
				return !state.editPending;
			},
			"Rig edit did not apply");
		return ReadObservedSetup(input.Snapshot().state);
	};
	const auto close = [](XrPosef a, XrPosef b) {
		return std::abs(a.position.x - b.position.x) + std::abs(a.position.y - b.position.y) +
				   std::abs(a.position.z - b.position.z) <
			   .0001f;
	};
	std::uint64_t previousFrame = 0;
	for (int frame = 0; frame < 8; ++frame)
	{
		rig.Move({0, 0, -1}, .04f, 0, .05f);
		const auto observed = submit();
		Require(observed.frame > previousFrame, "Rig observation did not advance");
		previousFrame = observed.frame;
		Require(close(observed.values.tracking.head.pose, rig.Head()) &&
					close(observed.values.tracking.controllers[0].pose.pose, rig.Hand(0)) &&
					close(observed.values.tracking.controllers[1].pose.pose, rig.Hand(1)),
				"Head and body-anchored hands did not arrive in one observed frame");
	}
	rig.SetHandAnchor(1, Anchor::World);
	const auto stationary = rig.Hand(1);
	for (int frame = 0; frame < 4; ++frame)
	{
		rig.Move({0, 0, -1}, -.04f, 0, .05f);
		const auto observed = submit();
		Require(close(observed.values.tracking.controllers[1].pose.pose, stationary),
				"World-anchored hand moved while walking");
		Require(close(observed.values.tracking.head.pose, rig.Head()), "Player stopped walking with a World anchor");
	}
	Require(input.Submit(ReleaseManualInputs{}), "Rig release refused");
	Eventually([&] { return !input.Snapshot().editPending; }, "Rig release did not complete");
	std::cout << "PASS native Player rig: 8 atomic walk/turn frames, 4 World-anchor frames\n";
}

#endif

void CheckPreviewPilot(Interaction& test)
{
	const bool remote = std::getenv("SIMXR_REMOTE_DRAG_QA") != nullptr;
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
			"Pilot observation stalled");
		return ReadObservedSetup(observer.Snapshot().state);
	};
	const auto key = [&](SDL_Scancode code, bool down, SDL_Keymod modifiers = SDL_KMOD_NONE) {
		SDL_Event event{};
		event.type = down ? SDL_EVENT_KEY_DOWN : SDL_EVENT_KEY_UP;
		event.key.windowID = SDL_GetWindowID(test.window.Native());
		event.key.scancode = code;
		event.key.key = SDL_GetKeyFromScancode(code, modifiers, false);
		event.key.mod = modifiers;
		event.key.down = down;
		Require(SDL_PushEvent(&event), "Push pilot key");
		pump(1);
	};
	const auto button = [&](unsigned code, bool down) {
		if (code == SDL_BUTTON_RIGHT && down)
		{
			const auto center = FindPanel("Image area")->InnerRect.GetCenter();
			SDL_Event pointer{};
			pointer.type = SDL_EVENT_MOUSE_MOTION;
			pointer.motion.windowID = SDL_GetWindowID(test.window.Native());
			pointer.motion.which = TestMouse;
			pointer.motion.x = center.x;
			pointer.motion.y = center.y;
			Require(SDL_PushEvent(&pointer), "Position capture pointer");
		}
		SDL_Event event{};
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.windowID = SDL_GetWindowID(test.window.Native());
		event.button.which = TestMouse;
		event.button.button = code;
		event.button.down = down;
		Require(SDL_PushEvent(&event), "Push pilot button");
		pump(1);
	};
	const auto motion = [&](float x, float y) {
		Require(test.window.Focused() && SDL_GetWindowRelativeMouseMode(test.window.Native()) == !remote,
				"Pilot motion lost focus or selected the wrong pointer mode");
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(test.window.Native());
		event.motion.which = TestMouse;
		event.motion.xrel = x;
		event.motion.yrel = y;
		Require(SDL_PushEvent(&event), "Push pilot motion");
		pump(1);
	};
	Require(SDL_ShowWindow(test.window.Native()), "Show isolated pilot window");
	SDL_RaiseWindow(test.window.Native());
#ifdef _WIN32
	if (std::getenv("SIMXR_SESSION_ZERO_INPUT"))
	{
		DWORD session = ~DWORD{0};
		Require(ProcessIdToSessionId(GetCurrentProcessId(), &session) && session == 0,
				"Synthetic focus is restricted to isolated Windows session 0");
		pump(3);
		Require(SDL_SetKeyboardFocus(test.window.Native()), "Inject isolated SDL keyboard focus");
		SDL_SetMouseFocus(test.window.Native());
		std::cout << "QA input uses injected SDL focus in session 0; physical foreground input is unqualified\n";
	}
#endif
	Eventually(
		[&] {
			pump(1);
			return test.window.Focused();
		},
		"Isolated pilot window did not receive focus");
	pump(3);
	auto* area = FindPanel("Image area");
	Require(area, "Image area missing for pilot");
	const auto center = area->InnerRect.GetCenter();
	SDL_WarpMouseInWindow(test.window.Native(), center.x, center.y);
	pump(3);
	const auto before = observed();
	{
		PlayerRig stale;
		stale.Reset(before);
		stale.position.x += 5;
		PilotController idle(stale);
		idle.Stop(nullptr, observer, false, true);
		Eventually(
			[&] {
				pump(1);
				return !observer.Snapshot().editPending;
			},
			"Idle release did not settle");
		const auto after = observed();
		Require(std::abs(after.values.tracking.head.pose.position.x - before.values.tracking.head.pose.position.x) <
					.001f,
				"Releasing buttons without a pose gesture submitted the stale rig pose");
	}
	if (remote)
	{
		const auto orientation = [&] {
			return observed().values.tracking.head.pose.orientation;
		};
		const auto different = [](XrQuaternionf a, XrQuaternionf b) {
			return std::abs(a.x - b.x) + std::abs(a.y - b.y) + std::abs(a.z - b.z) > .01f;
		};
		const auto start = orientation();
		button(SDL_BUTTON_RIGHT, true);
		pump(15);
		Require(SDL_CursorVisible(), "Remote drag hid the cursor");
		motion(60, 20);
		pump(5);
		Require(different(start, orientation()), "Remote drag did not rotate the runtime head");
		button(SDL_BUTTON_RIGHT, false);
		pump(5);
		const auto released = orientation();
		motion(60, 20);
		pump(5);
		Require(!different(released, orientation()), "Mouse-up retained remote piloting");
		for (const auto exit : {SDL_EVENT_KEY_DOWN, SDL_EVENT_WINDOW_MOUSE_LEAVE, SDL_EVENT_MOUSE_MOTION})
		{
			button(SDL_BUTTON_RIGHT, true);
			pump(15);
			key(SDL_SCANCODE_W, true);
			SDL_Event event{};
			event.type = exit;
			event.window.windowID = SDL_GetWindowID(test.window.Native());
			if (exit == SDL_EVENT_KEY_DOWN)
			{
				event.key.scancode = SDL_SCANCODE_ESCAPE;
				event.key.key = SDLK_ESCAPE;
				event.key.down = true;
			}
			if (exit == SDL_EVENT_MOUSE_MOTION)
			{
				event.motion.which = TestMouse;
				event.motion.x = -1;
				event.motion.xrel = 60;
			}
			Require(SDL_PushEvent(&event), "Push remote drag cancellation");
			pump(10);
			const auto stopped = observed().values.tracking.head.pose;
			motion(60, 20);
			pump(10);
			const auto after = observed().values.tracking.head.pose;
			Require(!different(stopped.orientation, after.orientation) &&
						std::abs(stopped.position.z - after.position.z) < .001f,
					"Remote drag cancellation retained motion or held keys");
			key(SDL_SCANCODE_W, false);
			key(SDL_SCANCODE_ESCAPE, false);
			button(SDL_BUTTON_RIGHT, false);
			pump(3);
		}
		test.renderer.WaitIdle();
		Require(test.validation->errors.load() == 0, "Remote drag validation errors");
		std::cout << "PASS remote drag: visible unlocked pointer, runtime head rotation, mouse-up, Escape and boundary "
					 "release\n";
		return;
	}
	ManualTransaction bodyClaim;
	bodyClaim.commands = {simxr::TakeControl{simxr::ControlChannel::Body, true}};
	Require(observer.Submit(bodyClaim), "Rival body claim refused");
	Eventually(
		[&] {
			pump(1);
			return !observer.Snapshot().editPending;
		},
		"Rival body claim did not settle");
	const auto rivalSource = observer.Snapshot().source;
	Require(rivalSource && observed().owners[static_cast<std::size_t>(simxr::ControlChannel::Body)] == rivalSource,
			"Rival did not acquire body ownership");
	pump(60);
	auto* handHud = FindPanel("HUD hand");
	Require(handHud && handHud->Active, "Pilot ownership HUD missing");
	if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "pilot-body-takeover.bmp");
		pump(1);
	}
	ImGui::ActivateItemByID(handHud->GetID("Take over"));
	pump(2);
	Eventually(
		[&] {
			const auto owner = observed().owners[static_cast<std::size_t>(simxr::ControlChannel::Body)];
			return owner && owner != rivalSource;
		},
		"Player HUD did not take over body ownership");
	std::cout << "PASS Player HUD body ownership takeover\n";
	button(SDL_BUTTON_RIGHT, true);
	Eventually(
		[&] {
			pump(1);
			return SDL_GetWindowRelativeMouseMode(test.window.Native());
		},
		"Preview did not capture pointer");
	const auto started = std::chrono::steady_clock::now();
	const auto updates = test.runtime.Snapshot().appliedPilotUpdates;
	key(SDL_SCANCODE_W, true);
	motion(50, 12);
	pump(20);
	key(SDL_SCANCODE_W, false);
	pump(5);
	const auto walked = observed();
	Require(std::abs(walked.values.tracking.head.pose.position.z - before.values.tracking.head.pose.position.z) > .04f,
			"Preview walk did not move player");
	const auto lookedBefore = simxr::Rotate(before.values.tracking.head.pose.orientation, {0, 0, -1});
	const auto lookedAfter = simxr::Rotate(walked.values.tracking.head.pose.orientation, {0, 0, -1});
	const auto lookChange =
		std::hypot(lookedAfter.x - lookedBefore.x, lookedAfter.y - lookedBefore.y, lookedAfter.z - lookedBefore.z);
	std::cout << "Preview head-look direction change: " << lookChange << '\n';
	Require(lookChange > .05f, "Preview relative mouse look did not rotate the head");
	const auto seconds = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::cout << "PASS preview applied-update rate: "
			  << (test.runtime.Snapshot().appliedPilotUpdates - updates) / seconds << " /s\n";
	key(SDL_SCANCODE_R, true);
	pump(5);
	Require(observed().values.input.controllers[1].primary, "Preview primary press missing");
	key(SDL_SCANCODE_R, false);
	button(SDL_BUTTON_LEFT, true);
	pump(5);
	Require(observed().values.input.controllers[1].trigger > .99f, "Preview trigger missing");
	button(SDL_BUTTON_LEFT, false);
	pump(5);
	key(SDL_SCANCODE_3, true);
	key(SDL_SCANCODE_3, false);
	const auto reachBefore = observed();
	const auto handBefore = reachBefore.values.tracking.controllers[1].pose.pose;
	key(SDL_SCANCODE_X, true);
	Require(ImGui::IsKeyDown(ImGuiKey_X) && !ImGui::GetIO().KeyAlt, "Pilot reach key was not held without Alt");
	motion(50, -25);
	pump(5);
	Require(SDL_GetWindowRelativeMouseMode(test.window.Native()) && ImGui::IsKeyDown(ImGuiKey_X),
			"Pilot reach lost capture or its held key");
	key(SDL_SCANCODE_X, false);
	const auto handAfter = observed().values.tracking.controllers[1].pose.pose;
	const auto viewBefore = simxr::RelativePose(handBefore, reachBefore.values.tracking.head.pose).position;
	const auto viewAfter = simxr::RelativePose(handAfter, reachBefore.values.tracking.head.pose).position;
	const XrVector3f reach{viewAfter.x - viewBefore.x, viewAfter.y - viewBefore.y, viewAfter.z - viewBefore.z};
	std::cout << "Preview reach in head view: " << reach.x << ", " << reach.y << ", " << reach.z << " m\n";
	Require(std::abs(reach.x - .1f) < .01f && std::abs(reach.y - .05f) < .01f && std::abs(reach.z) < .01f,
			"Preview reach did not move right hand right/up in the head view plane");
	key(SDL_SCANCODE_X, true);
	key(SDL_SCANCODE_LALT, true, SDL_KMOD_ALT);
	motion(35, 15);
	pump(5);
	key(SDL_SCANCODE_LALT, false);
	key(SDL_SCANCODE_X, false);
	const auto rotated = observed().values.tracking.controllers[1].pose.pose;
	Require(std::abs(rotated.orientation.y - handAfter.orientation.y) +
					std::abs(rotated.orientation.x - handAfter.orientation.x) >
				.01f,
			"Preview hand rotation did not rotate the right hand");
	motion(20, 0);
	pump(5);
	const auto turned = observed().values.tracking.controllers[1].pose.pose;
	Require(std::hypot(turned.position.x - rotated.position.x, turned.position.y - rotated.position.y,
					   turned.position.z - rotated.position.z) < .001f,
			"Target piloting reverted the hand reach");
	Require(std::abs(turned.orientation.y - rotated.orientation.y) > .01f,
			"Target piloting did not rotate the reached hand");
	key(SDL_SCANCODE_0, true);
	key(SDL_SCANCODE_0, false);
	std::cout << "PASS hand target motion preserves reach and rotation edits\n";
	const auto rootBefore = observed().values.tracking.body.rootPose;
	Require(rootBefore.has_value(), "Player did not publish body root");
	key(SDL_SCANCODE_LALT, true, SDL_KMOD_ALT);
	motion(40, 0);
	pump(5);
	key(SDL_SCANCODE_LALT, false);
	const auto looked = observed();
	Require(looked.values.tracking.body.rootPose &&
				std::abs(looked.values.tracking.body.rootPose->orientation.y - rootBefore->orientation.y) < .0001f,
			"Free look rotated body root");
	key(SDL_SCANCODE_R, true);
	pump(4);
	key(SDL_SCANCODE_ESCAPE, true);
	pump(5);
	Require(!SDL_GetWindowRelativeMouseMode(test.window.Native()), "Escape retained capture");
	Require(!observed().values.input.controllers[1].primary && observed().values.input.controllers[1].trigger == 0,
			"Escape retained held inputs");
	const auto escaped = observed();
	Require(std::abs(escaped.values.tracking.head.pose.position.z - looked.values.tracking.head.pose.position.z) <
				.001f,
			"Escape reset the player position");
	key(SDL_SCANCODE_ESCAPE, false);
	key(SDL_SCANCODE_R, false);
	button(SDL_BUTTON_RIGHT, false);
	pump(3);
	SDL_WarpMouseInWindow(test.window.Native(), center.x, center.y);
	pump(2);
	key(SDL_SCANCODE_LCTRL, true, SDL_KMOD_CTRL);
	button(SDL_BUTTON_RIGHT, true);
	Eventually(
		[&] {
			pump(1);
			return SDL_GetWindowRelativeMouseMode(test.window.Native());
		},
		"Locked capture did not start");
	button(SDL_BUTTON_RIGHT, false);
	key(SDL_SCANCODE_LCTRL, false);
	pump(3);
	Require(SDL_GetWindowRelativeMouseMode(test.window.Native()), "Capture lock ended on RMB release");
	key(SDL_SCANCODE_R, true);
	pump(5);
	Require(observed().values.input.controllers[1].primary, "Locked capture did not apply primary");
	SDL_Event focus{};
	focus.type = SDL_EVENT_WINDOW_FOCUS_LOST;
	focus.window.windowID = SDL_GetWindowID(test.window.Native());
	SDL_PushEvent(&focus);
	pump(5);
	Require(!SDL_GetWindowRelativeMouseMode(test.window.Native()) && !observed().values.input.controllers[1].primary,
			"Focus loss retained capture or held primary");
	focus.type = SDL_EVENT_WINDOW_FOCUS_GAINED;
	SDL_PushEvent(&focus);
	key(SDL_SCANCODE_R, false);
	pump(3);
	std::cout << "PASS native capture lock and focus-loss release\n";
	SDL_VirtualJoystickDesc padDescription{};
	SDL_INIT_INTERFACE(&padDescription);
	padDescription.type = SDL_JOYSTICK_TYPE_GAMEPAD;
	padDescription.naxes = SDL_GAMEPAD_AXIS_COUNT;
	padDescription.nbuttons = SDL_GAMEPAD_BUTTON_COUNT;
	padDescription.axis_mask = (1u << SDL_GAMEPAD_AXIS_COUNT) - 1;
	padDescription.button_mask = (1u << SDL_GAMEPAD_BUTTON_COUNT) - 1;
	padDescription.name = "SimXR isolated input QA";
	const auto padId = SDL_AttachVirtualJoystick(&padDescription);
	Require(padId != 0, "Attach virtual gamepad");
	auto* pad = SDL_OpenJoystick(padId);
	Require(pad, "Open virtual gamepad");
	SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFT_TRIGGER, -32768);
	SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, -32768);
	pump(4);
	SDL_SetJoystickVirtualButton(pad, SDL_GAMEPAD_BUTTON_SOUTH, true);
	SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_RIGHT_TRIGGER, 32767);
	SDL_SetJoystickVirtualAxis(pad, SDL_GAMEPAD_AXIS_LEFTX, 20000);
	pump(10);
	const auto gamepad = observed();
	const auto rawPad = test.window.ReadGamepad(GamepadBindings{});
	std::cout << "Gamepad QA observed primary=" << gamepad.values.input.controllers[1].primary
			  << " trigger=" << gamepad.values.input.controllers[1].trigger
			  << " stick=" << gamepad.values.input.controllers[0].stick.x << "; sampled connected=" << rawPad.connected
			  << " primary=" << rawPad.controls[1].primary << " trigger=" << rawPad.controls[1].trigger
			  << " stick=" << rawPad.controls[0].stick.x << "\n";

	Require(gamepad.values.input.controllers[1].primary && gamepad.values.input.controllers[1].trigger > .99f &&
				gamepad.values.input.controllers[0].stick.x > .4f,
			"Gamepad button, trigger or thumbstick did not reach runtime");
	test.runtime.PrepareReplay();
	Require(!test.runtime.ReplayReady(), "Replay accepted held gamepad ownership before releasing it");
	Eventually(
		[&] {
			pump(1);
			return test.runtime.ReplayReady();
		},
		"Held gamepad prevented replay input release");
	pump(4);
	const auto releasedForReplay = observed();
	std::cout << "Replay preparation ready=" << test.runtime.ReplayReady()
			  << " operationPending=" << test.runtime.Snapshot().operationPending
			  << " error=" << test.runtime.Snapshot().error << " owners:";
	for (std::size_t channel = 0; channel < releasedForReplay.owners.size(); ++channel)
		if (const auto& owner = releasedForReplay.owners[channel])
			std::cout << " " << channel << "=" << static_cast<unsigned>(owner->kind) << ":" << owner->id;
	std::cout << '\n';
	Require(
		test.runtime.ReplayReady() &&
			std::none_of(releasedForReplay.owners.begin(), releasedForReplay.owners.end(),
						 [](const auto& owner) { return owner && owner->kind == simxr::CommandSourceKind::Manual; }),
		"Held gamepad reclaimed inputs during replay preparation");
	test.runtime.FinishReplayPreparation();
	pump(10);
	Require(observed().values.input.controllers[1].primary,
			"Ending replay preparation did not restore ordinary gamepad input");
	std::cout << "PASS held gamepad remains released throughout replay preparation\n";
	key(SDL_SCANCODE_LCTRL, true, SDL_KMOD_CTRL);
	button(SDL_BUTTON_RIGHT, true);
	Eventually(
		[&] {
			pump(1);
			return SDL_GetWindowRelativeMouseMode(test.window.Native());
		},
		"Capture with connected gamepad did not start");
	button(SDL_BUTTON_RIGHT, false);
	key(SDL_SCANCODE_LCTRL, false);
	key(SDL_SCANCODE_R, true);
	SDL_CloseJoystick(pad);
	Require(SDL_DetachVirtualJoystick(padId), "Detach virtual gamepad");
	pump(10);
	const auto unplugged = observed();
	Require(!SDL_GetWindowRelativeMouseMode(test.window.Native()), "Gamepad unplug retained locked capture");
	Require(!unplugged.values.input.controllers[1].primary && unplugged.values.input.controllers[1].trigger == 0 &&
				unplugged.values.input.controllers[0].stick.x == 0,
			"Gamepad unplug left held input");
	key(SDL_SCANCODE_R, false);
	std::cout << "PASS native gamepad mapping and unplug releases held keyboard input and capture\n";
	test.workspace.Select(DeviceSel{DevicePage::Head});
	pump(4);
	const auto headBeforeRecenter = observed().values.tracking.head.pose;
	ImGui::ActivateItemByID(FindPanel("Inspector")->GetID("Recenter view"));
	pump(2);
	Eventually(
		[&] {
			const auto state = observed();
			const auto& local = state.values.referenceSpaces.local;
			const auto& head = state.values.tracking.head.pose;
			Require(std::abs(head.position.x - headBeforeRecenter.position.x) < .001f &&
						std::abs(head.position.y - headBeforeRecenter.position.y) < .001f &&
						std::abs(head.position.z - headBeforeRecenter.position.z) < .001f,
					"Recenter moved the head");
			return std::abs(local.position.x - head.position.x) < .001f &&
				   std::abs(local.position.y - head.position.y) < .001f &&
				   std::abs(local.position.z - head.position.z) < .001f;
		},
		"Head page recenter did not update the local reference space");
	std::cout << "PASS Head page recenter updates reference space without moving the head\n";
	SDL_SetEventFilter(nullptr, nullptr);
	Require(test.validation->errors.load() == 0, "Validation during preview piloting");
	std::cout << "PASS native preview walk, look, reach, primary, trigger and Escape release\n";
}

static void CheckReturnToImage(Interaction& test)
{
	test.workspace.Select(DeviceSel{DevicePage::Player});
	for (int i = 0; i < 3; ++i)
		test.Frame();
	for (int action = 0; action < 4; ++action)
	{
		const bool recorded = action >= 2, thumbnail = action % 2 != 0;
		if (action == 2)
		{
			test.runtime.Pause();
			Eventually(
				[&] {
					test.Frame();
					const auto state = test.runtime.Snapshot();
					return state.manual && !state.operationPending;
				},
				"Return image fixture did not pause");
			test.runtime.Step();
			Eventually(
				[&] {
					test.Frame();
					const auto state = test.runtime.Snapshot();
					return !state.operationPending && state.selectedReady && state.selectedCapture &&
						   state.inspectedFrame == state.selectedFrame;
				},
				"Recorded image did not load");
			const auto state = test.runtime.Snapshot();
			test.workspace.Select(FrameSel{state.selectedIdentity, state.selectedEpoch});
			test.Frame();
		}
		auto* inspector = FindPanel(recorded ? "Frame details" : "Inspector");
		Require(inspector, "Rig inspector missing");
		ImGui::ActivateItemByID(ImHashStr("Expand", 0, inspector->GetID(recorded ? "Recorded rig" : "Rig view")));
		for (int i = 0; i < 3; ++i)
			test.Frame();
		const auto state = test.runtime.Snapshot();
		Require(state.rigExpanded, recorded ? "Recorded rig did not expand" : "Live rig did not expand");
		const auto* controls = FindPanel("Return to image");
		Require(controls, "Return controls missing");
		const float buttonHeight = ImGui::GetFrameHeight();
		const float x = controls->Pos.x + controls->Size.x / 2;
		const float y = controls->Pos.y +
						(thumbnail ? buttonHeight + 8 + (controls->Size.y - buttonHeight - 8) / 2 : buttonHeight / 2);
		test.Click({x, y});
		test.Frame();
		Require(!test.runtime.Snapshot().rigExpanded,
				thumbnail ? "Thumbnail click did not restore image" : "Back to image click did not restore image");
	}
	test.runtime.ResumeLive();
	Eventually(
		[&] {
			test.Frame();
			return !test.runtime.Snapshot().manual && !test.runtime.Snapshot().operationPending;
		},
		"Return to image fixture did not resume");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Return to image validation errors");
	std::cout << "PASS live and recorded rigs restore image from button and thumbnail clicks\n";
}

static void CheckCrop(Interaction& test)
{
	test.runtime.PreviewCrop() = ImVec4{.2f, .2f, .8f, .8f};
	test.Frame();
	test.Frame();
	const auto directory = QaDirectory("SIMXR_UI_QA_DIR");
	Require(!directory.empty(), "Crop QA needs an output directory");
	test.renderer.RequestScreenshot(directory / "crop-control.bmp");
	test.Frame();
	auto* controls = FindPanel("Image controls");
	Require(controls, "Crop controls missing");
	ImGui::ActivateItemByID(controls->GetID("Reset crop"));
	test.Frame();
	Require(!test.runtime.PreviewCrop(), "Reset crop did not restore the whole image");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Crop UI validation errors");
	std::cout << "PASS plain crop button restores the whole image\n";
}

void CheckConnectedPreview(const char* endpoint)
{
	if (std::getenv("SIMXR_REMOTE_DRAG_QA"))
	{
		const auto path = QaDirectory("SIMXR_COMPANION_SETTINGS");
		Require(!path.empty(), "Remote drag QA needs private settings");
		auto settings = LoadSettings(path);
		settings.remoteMouseDrag = true;
		SaveSettings(path, settings);
	}

	if (std::getenv("SIMXR_HISTORY_QA"))
	{
		const auto path = QaDirectory("SIMXR_COMPANION_SETTINGS");
		Require(!path.empty(), "History QA needs private settings");
		auto settings = LoadSettings(path);
		settings.previewBudgetMiB = 64;
		settings.previewRamMiB = 64;
		SaveSettings(path, settings);
	}

	Interaction test(std::getenv("SIMXR_REMOTE_DRAG_QA") != nullptr);
	test.live = true;
	test.runtime.ConnectEndpoint(endpoint);
	Eventually(
		[&] {
			test.Frame();
			return test.runtime.Snapshot().selectedReady;
		},
		"Existing isolated app did not supply a preview");
#ifdef __linux__
	if (std::getenv("SIMXR_LAYOUT_QA"))
	{
		const auto directory = std::getenv("SIMXR_UI_QA_DIR");
		Require(directory != nullptr, "Connected layout checks need an output directory");
		CheckLayouts(test, directory);
	}
	else
#endif
	{
		if (std::getenv("SIMXR_RETURN_IMAGE_QA"))
			CheckReturnToImage(test);
		else if (std::getenv("SIMXR_CROP_QA"))
			CheckCrop(test);
		else if (std::getenv("SIMXR_VIDEO_QA"))
			CheckVideoExport(test);
		else if (std::getenv("SIMXR_HISTORY_QA"))
			CheckPreviewHistory(test);
#ifdef __linux__
		else if (std::getenv("SIMXR_SPATIAL_QA"))
			CheckSpatialEditing(test);
		else
			CheckPreviewPilot(test);
#else
		else
		{
			Require(!std::getenv("SIMXR_LAYOUT_QA") && !std::getenv("SIMXR_SPATIAL_QA"),
					"Connected layout/spatial fixtures are not enabled on this platform");
			CheckPreviewPilot(test);
		}
#endif
	}
	if (const auto directory = QaDirectory("SIMXR_UI_QA_DIR"); !directory.empty())
	{
		test.renderer.RequestScreenshot(directory / "application-gameplay.bmp");
		test.Frame();
		test.renderer.WaitIdle();
	}
}

}
