#include "../TestSupport.h"
#include "Math.h"
#include "app/DeviceControlPanel.h"
#include "app/ManualInputController.h"
#include "app/ProcessSupervisor.h"
#include "app/RecordingController.h"
#include "app/RuntimeConnection.h"
#include "app/RuntimePanels.h"
#include "app/HistoryStore.h"
#include "app/TimelineCanvas.h"
#include <functional>
#include "app/SimulationEdits.h"
#include "app/SpatialMath.h"
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
#include <fstream>
#include <imgui.h>
#include <imgui_internal.h>
#include <iostream>
#include <set>
#include <sstream>

namespace
{
using namespace simxr::app;

ImGuiWindow* FindPanel(const char* name)
{
	if (auto* exact = ImGui::FindWindowByName(name))
		return exact;
	std::string leaf = name;
	if (leaf == "Device controls" || leaf == "Recording" || leaf == "Rigs" || leaf == "Historical state" ||
		leaf == "Action and binding inspector" || leaf == "Captures" || leaf == "Frame controls")
		leaf = "Inspector";
	if (leaf == "##Workspace toolbar")
		leaf = "Top bar";
	if (leaf == "##Frame timeline")
		leaf = "Timeline";
	const std::string pattern = "/" + leaf + "_";
	for (auto* window : ImGui::GetCurrentContext()->Windows)
		if (std::string_view(window->Name).find(pattern) != std::string_view::npos)
			return window;
	return nullptr;
}
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
	std::function<void()> extraDraw;
	unsigned clicks = 0;
	bool live = false;
	Interaction(bool launch = false) : workspace(processes, launch, ""), live(launch) {}

	void Frame()
	{
		Eventually(
			[&]
			{
				Require(window.PollEvents(), "Unexpected window closure");
				return renderer.BeginFrame();
			},
			"UI frame admission stalled");
		if (live)
		{
			if (window.TakeFocusLost())
				runtime.FocusLost();
			runtime.Poll();
		}
		workspace.BeginFrame(runtime);
		if (live)
		{
			workspace.Draw(runtime);
			const auto transfers = [&](VkCommandBuffer command)
			{
				runtime.RecordTransfers(command);
			};
			Require(renderer.Render(runtime.ReadyWaits(), transfers), "Live launch UI frame was not rendered");
			runtime.Submitted(renderer.SubmittedFrames());
			return;
		}
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
		if (extraDraw) extraDraw();
		const auto transfers = [&](VkCommandBuffer command)
		{
			runtime.RecordTransfers(command);
		};
		Require(renderer.Render(runtime.ReadyWaits(), transfers), "Admitted UI frame was not rendered");
		runtime.Submitted(renderer.SubmittedFrames());
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
	void SelectWorkspace(Selection selection)
	{
		workspace.Select(std::move(selection));
		Frame();
		Require(FindPanel("Inspector") != nullptr, "Inspector is missing");
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
		workspace.Select(DeviceSel{});
		Frame();
		const auto before = clicks;
		Click(buttonCenter);
		Require(clicks == before + 1, "Other controls stopped responding after selection");
		workspace.Select(LibrarySel{});
		Frame();
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
void CheckTimelineCanvas(Interaction& test)
{
	HistoryStore history;
	for (std::uint64_t frame = 1; frame <= 54000; ++frame)
		history.Append(
			{{1, 1, frame}, 1, 0, double(frame) / 90, 11.11f, (frame == 200 ? HistoryDropped : 0U) | HistoryMarker});
	HistoryStore emptyHistory;
	bool populated = true;
	double canvasSeconds = 0;
	TimelineCanvas canvas;
	canvas.Fit();
	TimelineGesture selected;
	ImVec2 point;
	test.extraDraw = [&]
	{
		ImGui::SetNextWindowPos({30, 170}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({740, 240}, ImGuiCond_Always);
		ImGui::Begin("Timeline interaction");
		const auto top = ImGui::GetCursorScreenPos();
		const auto available = ImGui::GetContentRegionAvail();
		const float labels = std::min(available.x * .25f, ImGui::GetFontSize() * 6);
		point = {top.x + labels + (available.x - labels) * .5f, top.y + 10};
		const auto drawStarted = std::chrono::steady_clock::now();
		canvas.Begin(labels, 0);
		const auto gesture = canvas.Draw(populated ? history : emptyHistory, false);
		canvasSeconds += std::chrono::duration<double>(std::chrono::steady_clock::now() - drawStarted).count();
		if (gesture.first || gesture.marker)
			selected = gesture;
		ImGui::End();
	};
	for (int i = 0; i < 3; ++i)
		test.Frame();
	test.Click(point);
	Require(selected.first && !selected.last && *selected.first > 26000 && *selected.first < 28000,
			"Canvas click did not hit-test ten-minute history");
	SDL_Event key{};
	key.type = SDL_EVENT_KEY_DOWN;
	key.key.windowID = SDL_GetWindowID(test.window.Native());
	key.key.key = SDLK_M;
	key.key.scancode = SDL_SCANCODE_M;
	key.key.down = true;
	SDL_PushEvent(&key);
	test.Frame();
	key.type = SDL_EVENT_KEY_UP;
	key.key.down = false;
	SDL_PushEvent(&key);
	test.Frame();
	Require(selected.marker.has_value(), "M did not mark the hovered timeline frame");
	populated = false;
	const auto baselineStarted = std::chrono::steady_clock::now();
	for (int i = 0; i < 60; ++i)
		test.Frame();
	const double baseline = std::chrono::duration<double>(std::chrono::steady_clock::now() - baselineStarted).count();
	populated = true;
	canvasSeconds = 0;
	const auto started = std::chrono::steady_clock::now();
	for (int i = 0; i < 120; ++i)
	{
		canvas.Zoom(i * 4, i * 4 + 120);
		test.Frame();
	}
	const double elapsed = std::chrono::duration<double>(std::chrono::steady_clock::now() - started).count();
	std::cout << "PASS native canvas hit-testing and marker; 54000 records, " << 120 / elapsed << " UI frames/s; empty "
			  << 60 / baseline << "; canvas CPU " << canvasSeconds * 1000 / 120 << " ms/frame\n";
	test.extraDraw = {};
	test.Frame();
}

void CheckTimelineWorkflow(Interaction& test)
{
	test.workspace.SetScale(1);
	test.workspace.Select(FrameSel{});
	const auto activate = [&](const char* label)
	{
		auto* timeline = FindPanel("Timeline");
		Require(timeline != nullptr, "Timeline missing");
		ImGui::ActivateItemByID(timeline->GetID(label));
		test.Frame();
	};
	const auto await = [&](auto predicate, const char* failure)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		do
		{
			test.Frame();
			if (predicate(test.runtime.Snapshot()))
				return;
		} while (std::chrono::steady_clock::now() < deadline);
		throw std::runtime_error(failure);
	};
	test.Frame();
	activate("Pause");
	await(
		[](const auto& state)
		{
			return state.manual && !state.operationPending;
		},
		"Timeline pause failed");
	RuntimeView earlier;
	for (int step = 0; step < 3; ++step)
	{
		const auto previous = test.runtime.Snapshot().selectedCapture;
		activate("Next");
		await(
			[&](const auto& state)
			{
				return !state.operationPending && state.selectedReady && state.selectedCapture != previous &&
					   state.inspectedFrame == state.selectedFrame;
			},
			"Timeline step did not retain image and CPU state");
		if (!step)
			earlier = test.runtime.Snapshot();
	}
	const auto newest = test.runtime.Snapshot();
	Require(newest.time.following, "Next frame stopped following the paused live edge");
	await(
		[&](const auto&)
		{
			return test.runtime.History().Find(earlier.selectedIdentity, earlier.selectedEpoch).has_value() &&
				   test.runtime.History().Find(newest.selectedIdentity, newest.selectedEpoch).has_value();
		},
		"Snapshot frame absent from timeline");
	Require(std::holds_alternative<FrameSel>(test.workspace.State().selection) &&
				std::get<FrameSel>(test.workspace.State().selection).identity == newest.selectedIdentity,
			"Next frame did not select the new frame");
	const auto index = *test.runtime.History().Find(earlier.selectedIdentity, earlier.selectedEpoch);
	Require(test.runtime.History().At(index).flags & HistorySnapshot, "Images lane omitted retained snapshot");
	test.Frame();
	const auto bounds = test.workspace.State().timelineCanvas;
	ImGui::GetIO().AddMousePosEvent(bounds.x + bounds.width - 1, bounds.y + 2);
	test.Frame();
	ImGui::GetIO().AddMouseWheelEvent(0, 8);
	test.Frame();
	test.Frame();
	const auto& shell = test.workspace.State();
	const double seconds = test.runtime.History().At(index).seconds;
	const float x =
		shell.timelineCanvas.x +
		float((seconds - shell.timeline.first) / (shell.timeline.last - shell.timeline.first)) * shell.timelineCanvas.width;
	Require(x >= shell.timelineCanvas.x && x <= shell.timelineCanvas.x + shell.timelineCanvas.width,
			"Earlier snapshot not visible after timeline zoom");
	test.Click({x, shell.timelineCanvas.y + shell.timelineCanvas.height * .35f});
	await(
		[&](const auto& state)
		{
			return state.manual && !state.time.following && state.selectedFrame == earlier.selectedFrame &&
				   state.selectedReady && state.inspectedFrame == earlier.selectedFrame;
		},
		"Canvas did not select earlier image and inputs/actions");
	Require(std::holds_alternative<FrameSel>(test.workspace.State().selection),
			"Canvas selection did not open Frame page");
	const auto* details = FindPanel("Frame details");
	Require(details != nullptr, "Merged Frame page absent");
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot((std::filesystem::path(directory) / "timeline-paused.bmp").string());
		test.Frame();
		test.Frame();
		ImGui::SetScrollY(const_cast<ImGuiWindow*>(details), 410);
		test.Frame();
		test.Frame();
		test.renderer.RequestScreenshot((std::filesystem::path(directory) / "frame-input-actions.bmp").string());
		test.Frame();
		test.Frame();
	}
	// Select a range through the ruler, then use its inspector action.
	const auto beforeRange = test.workspace.State();
	const auto rectangle = beforeRange.timelineCanvas;
	const float rangeStart = x - 1, rangeEnd = rectangle.x + rectangle.width - 1;
	auto& io = ImGui::GetIO();
	io.AddMousePosEvent(rangeStart, rectangle.y + 2);
	test.Frame();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, true);
	test.Frame();
	io.AddMousePosEvent(rangeEnd, rectangle.y + 2);
	test.Frame();
	io.AddMouseButtonEvent(ImGuiMouseButton_Left, false);
	test.Frame();
	test.Frame();
	Require(std::holds_alternative<RangeSel>(test.workspace.State().selection), "Ruler drag did not open Range page");
	auto* inspector = FindPanel("Inspector");
	ImGui::ActivateItemByID(inspector->GetID("Zoom to range"));
	test.Frame();
	test.Frame();
	Require(test.workspace.State().timeline.last - test.workspace.State().timeline.first <
				beforeRange.timeline.last - beforeRange.timeline.first,
			"Range zoom did not narrow the timeline");
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot((std::filesystem::path(directory) / "timeline-range.bmp").string());
		test.Frame();
		test.Frame();
	}
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		const auto destination = std::filesystem::path(directory)/"range.csv";
		ImGui::ActivateItemByID(inspector->GetID("##Range export file"));
		ImGui::GetCurrentContext()->NavNextActivateFlags = ImGuiActivateFlags_PreferInput;
		test.Frame();
		io.AddKeyEvent(ImGuiMod_Ctrl, true); io.AddKeyEvent(ImGuiKey_A, true);
		test.Frame();
		io.AddKeyEvent(ImGuiKey_A, false); io.AddKeyEvent(ImGuiMod_Ctrl, false);
		io.AddInputCharactersUTF8(destination.string().c_str());
		test.Frame();
		io.AddKeyEvent(ImGuiKey_Enter, true); test.Frame();
		io.AddKeyEvent(ImGuiKey_Enter, false); test.Frame();
		ImGui::ActivateItemByID(inspector->GetID("Export frames"));
		test.Frame();
		await([&](const auto& state)
		{
			std::size_t images = 0;
			for (const auto& file : std::filesystem::directory_iterator(directory))
				if (file.path().filename().string().starts_with("range-frame-") && file.path().extension()==".ppm" && file.file_size()>100) ++images;
			return !state.pending && std::filesystem::exists(destination) && images>=3;
		}, "Range export did not save metadata and three snapshots");
	}
	activate("Continue");
	await(
		[](const auto& state)
		{
			return !state.manual && state.time.following && !state.operationPending;
		},
		"Continue did not return to LIVE");
	Require(test.runtime.Snapshot().error.empty(), "Timeline workflow reported runtime error");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0,
			"Timeline snapshot/thumbnail workflow produced Vulkan validation errors");
	std::cout << "PASS timeline pause, three steps, canvas click, historical image/input/actions, range "
				 "selection/zoom, Continue to LIVE\n";
}

void CheckShell()
{
	Interaction test;
	for (int i = 0; i < 4; ++i)
		test.Frame();
	const auto preview = FindPanel("GPU preview");
	const auto inspector = FindPanel("Inspector");
	Require(preview && inspector && !preview->DockId && !inspector->DockId, "Shell still uses docks");
	Require(preview->Pos.x + preview->Size.x <= inspector->Pos.x + 1, "Inspector overlaps the wide preview");
	for (int index = 0; index < 9; ++index)
	{
		auto* rail = FindPanel("Device rail");
		Require(rail != nullptr, "Rail missing");
		test.Click({rail->Pos.x + 12, rail->Pos.y + ImGui::GetStyle().WindowPadding.y +
										  index * (36 * ImGui::GetFontSize() / 17 + ImGui::GetStyle().ItemSpacing.y) + 18});
		const auto& selection = test.workspace.State().selection;
		if (index < 7)
			Require(std::holds_alternative<DeviceSel>(selection) &&
						static_cast<int>(std::get<DeviceSel>(selection).page) == index,
					("Rail device selection failed at " + std::to_string(index)).c_str());
		else
			Require(std::holds_alternative<SessionItemSel>(selection), "Session selection failed");
	}
	const auto width = preview->Size.x;
	test.workspace.SetPreviewExpanded(true);
	test.Frame();
	test.Frame();
	Require(FindPanel("GPU preview")->Size.x > width, "Full preview did not expand");
	test.workspace.SetPreviewExpanded(false);
	test.Frame();
	test.Frame();
	Require(std::abs(FindPanel("GPU preview")->Size.x - width) < 1, "Preview restore changed layout");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Shell generated validation errors");
}
// Opt-in walkthrough against a separately launched application, never the user's session.
void PilotLiveRuntime(const char* endpoint)
{
	Interaction test;
	test.runtime.ConnectEndpoint(endpoint);
	test.workspace.Select(DeviceSel{});
	const auto frame = [&]
	{
		Eventually(
			[&]
			{
				return test.window.PollEvents() && test.renderer.BeginFrame();
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
			return test.runtime.Snapshot().selectedReady;
		},
		"Application did not supply a live preview");
	const auto activate = [&](const char* window, const char* label)
	{
		auto* target = FindPanel(window);
		Require(target != nullptr, "Control window is missing");
		ImGui::SetWindowFocus(target->Name);
		ImGui::ActivateItemByID(target->GetID(label));
		frame();
	};
	Require(std::getenv("SIMXR_COMPANION_SETTINGS"), "Isolated recording checks need a dedicated settings path");
	test.workspace.Select(LibrarySel{});
	for (int i = 0; i < 4; ++i)
		frame();
	Eventually(
		[&]
		{
			frame();
			return !test.runtime.Snapshot().recordingBusy;
		},
		"Initial recording read did not finish");
	activate("##Workspace toolbar", "Rec");
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
	auto* rail = FindPanel("Device rail");
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
	const ImVec2 handTab{rail->Pos.x + 12, rail->Pos.y + 2 * (28 + ImGui::GetStyle().ItemSpacing.y) + 10};
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
	Eventually(
		[&]
		{
			frame();
			return !test.runtime.Snapshot().recordingBusy;
		},
		"Recording refresh did not finish");
	activate("##Workspace toolbar", "Stop recording");
	Eventually(
		[&]
		{
			frame();
			return !test.runtime.Snapshot().recording && !test.runtime.Snapshot().recordingBusy;
		},
		"Stop recording failed outside the recording workspace");
	Require(test.runtime.Snapshot().recordedChanges > 0, "Real piloting produced no recorded input");
	Require(!popups.empty() && popups.back().Window, "Stop did not open the save popover");
	ImGui::ActivateItemByID(popups.back().Window->GetID("Save"));
	std::string savedPath;
	Eventually(
		[&]
		{
			frame();
			if (const auto take = std::get_if<TakeSel>(&test.workspace.State().selection))
				savedPath = take->id;
			return !savedPath.empty() && std::filesystem::exists(savedPath);
		},
		"Save take did not produce a persistent file");
	Require(!OpenSavedRecording(savedPath).path.empty(), "Saved take cannot be reopened independently");
	for (int i = 0; i < 4; ++i)
		frame();
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot((std::filesystem::path(directory) / "cemu-recording-saved.bmp").string());
		frame();
		frame();
	}
	activate("Inspector", "Replay take");
	Eventually(
		[&]
		{
			frame();
			return test.runtime.Snapshot().manual;
		},
		"Saved take did not enter replay");
	test.workspace.Select(DeviceSel{});
	for (int i = 0; i < 3; ++i)
		frame();
	activate("##Workspace toolbar", "Cancel replay");
	Eventually(
		[&]
		{
			frame();
			return !test.runtime.Snapshot().manual;
		},
		"Cancelling replay did not release frame control");
	std::cout << "PASS Cemu record while piloting, global stop, save, independent reopen, replay and global cancel\n";

	// Exercise the inspection context on the same isolated real application.
	test.workspace.Select(FrameSel{});
	activate("##Frame timeline", "Pause");
	Eventually(
		[&]
		{
			frame();
			return test.runtime.Snapshot().manual && !test.runtime.Snapshot().operationPending;
		},
		"Application did not pause");
	activate("##Frame timeline", "Next");
	Eventually(
		[&]
		{
			frame();
			const auto state = test.runtime.Snapshot();
			return !state.operationPending && state.selectedCapture && state.selectedReady && state.time.following;
		},
		"Step did not select its captured frame");
	const auto retained = test.runtime.Snapshot().selectedCapture;
	activate("##Frame timeline", "Next");
	Eventually(
		[&]
		{
			frame();
			const auto state = test.runtime.Snapshot();
			return !state.operationPending && state.selectedReady && state.selectedCapture != retained;
		},
		"Timeline next frame did not capture another snapshot");
	activate("##Frame timeline", "Previous snapshot");
	Require(test.runtime.Snapshot().selectedCapture == retained && test.runtime.Snapshot().manual,
			"Timeline did not select the earlier snapshot while paused");

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
	test.workspace.SetScale(1.5f);
	for (int i = 0; i < 4; ++i)
		frame();
	captureContext("cemu-timeline-150.bmp");
	activate("##Frame timeline", "Rig events");
	captureContext("cemu-timeline-events.bmp");
	SDL_Event escape{};
	escape.type = SDL_EVENT_KEY_DOWN;
	escape.key.windowID = SDL_GetWindowID(test.window.Native());
	escape.key.key = SDLK_ESCAPE;
	escape.key.scancode = SDL_SCANCODE_ESCAPE;
	escape.key.down = true;
	SDL_PushEvent(&escape);
	frame();
	escape.type = SDL_EVENT_KEY_UP;
	escape.key.down = false;
	SDL_PushEvent(&escape);
	frame();
	test.workspace.SetScale(1.0f);
	for (int i = 0; i < 4; ++i)
		frame();
	activate("##Frame timeline", "Continue");
	Eventually(
		[&]
		{
			frame();
			const auto state = test.runtime.Snapshot();
			return !state.operationPending && !state.manual && state.time.following;
		},
		"Continue did not resume and follow live output");
	activate("##Frame timeline", "Previous snapshot");
	activate("##Frame timeline", "Previous snapshot");
	Require(test.runtime.Snapshot().selectedCapture == retained,
			"Timeline did not return to the historical snapshot while running");
	for (int i = 0; i < 6; ++i)
		frame();
	Require(!test.runtime.Snapshot().manual && !test.runtime.Snapshot().time.following,
			"Inspecting a retained frame falsely paused the application");
	captureContext("cemu-inspection-running.bmp");
	test.runtime.SetLive(true);
	frame();
	Require(test.runtime.Snapshot().time.following && !test.runtime.Snapshot().manual,
			"Showing live output changed the application pause state");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors in frame inspection and thumbnail rendering");
	std::cout << "PASS real application inspection: pause, next frame, captured image, continue and historical view "
				 "while running\n";
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
	Selection selection = SessionItemSel{SessionItem::Headset};
	std::optional<SimulationEdit> edit;
	const auto frame = [&]
	{
		Eventually(
			[&]
			{
				return test.window.PollEvents() && test.renderer.BeginFrame();
			},
			"Live field UI stalled");
		ImGui::SetNextWindowPos({20, 20}, ImGuiCond_Always);
		ImGui::SetNextWindowSize({400, 650}, ImGuiCond_Always);
		ImGui::Begin("Configuration workflow");
		edit = panel.Draw(true, &selection);
		ImGui::End();
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
	const auto typeValue = [&](const char* label, const char* value)
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
		const auto changed = edit;
		io.AddKeyEvent(ImGuiKey_Enter, true);
		frame();
		io.AddKeyEvent(ImGuiKey_Enter, false);
		frame();
		Require(changed && std::holds_alternative<ManualTransaction>(*changed), "Typing did not submit a live value");
		return std::get<ManualTransaction>(*changed);
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
	transaction = activate("Tracker tracked");
	Require(std::get<simxr::SetTracker>(transaction.commands.back()).value.identity ==
				initial.tracking.trackers[0].identity,
			"Live tracker edit changed identity");
	selection = SessionItemSel{SessionItem::Scenarios};
	frame();
	transaction = activate("Activate scenario");
	simxr::CommandTransaction command;
	command.id = 1;
	command.source = {simxr::CommandSourceKind::Manual, 77};
	command.target.frame = simxr::FrameId{2};
	command.commands = transaction.commands;
	scheduler.Submit(command);
	observe(2);
	panel.ObserveDelivery(false, {});
	frame();
	transaction = activate("Restore normal behavior");
	Require(!std::get<simxr::SetScenario>(transaction.commands.back()).enabled,
			"Scenario recovery did not clear failure");
	command.id = 2;
	command.target.frame = simxr::FrameId{3};
	command.commands = transaction.commands;
	scheduler.Submit(command);
	observe(3);
	panel.ObserveDelivery(false, {});
	frame();
	panel.ResetObservations();
	frame();
	ImGui::ActivateItemByID(window->GetID("Activate scenario"));
	frame();
	Require(!edit, "Disconnected field submitted stale settings");
	initial.views.ipd = .061f;
	scheduler.Start(0, initial);
	observe(1);
	frame();
	selection = SessionItemSel{SessionItem::Headset};
	frame();
	transaction = typeValue("##IPD", "0.063");
	Require(std::abs(std::get<simxr::SetViewSettings>(transaction.commands.back()).value.ipd - .063f) < .0001f,
			"Reconnected field did not send the new value");
	test.renderer.WaitIdle();
	Require(!test.validation->errors.load(), "Live fields generated validation errors");
	std::cout << "PASS live configuration: immediate changes, preserved configuration, scenario recovery, reconnect\n";
}

#ifdef __linux__
void CheckDebuggerWorkflow(const char* endpoint)
{
	const auto destination = std::getenv("SIMXR_DEBUG_QA_DIRECTORY");
	Require(destination, "Debugger QA requires a new private output directory");
	const std::filesystem::path directory(destination);
	Require(directory.is_absolute() && std::filesystem::create_directory(directory), "Debugger output must be new");
	Require(::setenv("SIMXR_COMPANION_SETTINGS", (directory / "companion.settings").c_str(), 1) == 0,
			"Isolate debugger settings");
	Interaction test(true);
	test.runtime.ConnectEndpoint(endpoint);
	const auto wait = [&](const auto& ready, const char* failure)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
		do
		{
			test.Frame();
			if (std::chrono::steady_clock::now() >= deadline)
			{
				const auto state = test.runtime.Snapshot();
				throw std::runtime_error(std::string(failure) + ": " + state.phase + "/" + state.progress + " " +
										 state.error);
			}
		} while (!ready(test.runtime.Snapshot()));
	};
	wait(
		[](const auto& state)
		{
			return state.selectedReady;
		},
		"Debugger did not attach live output");
	test.SelectWorkspace(FrameSel{});
	test.runtime.SetLive(false);
	test.runtime.Command(simxr::control::CommandKind::Pause);
	wait(
		[](const auto& state)
		{
			return !state.operationPending && state.progress == "paused";
		},
		"Pause failed");
	test.runtime.Command(simxr::control::CommandKind::StrictAdmission);
	wait(
		[](const auto& state)
		{
			return !state.operationPending && state.progress == "issued" && state.selectedReady &&
				   state.selectedImageCapture == state.selectedCapture;
		},
		"One admitted frame did not produce a retained image");
	const auto exact = test.runtime.Snapshot();
	test.runtime.InspectSelected();
	wait(
		[&](const auto& state)
		{
			return state.inspectedFrame == exact.selectedFrame;
		},
		"Exact state inspection failed");
	test.runtime.RequestExport((directory / "composed.ppm").string());
	wait(
		[&](const auto& state)
		{
			return !state.pending && std::filesystem::exists(directory / "composed.ppm");
		},
		"Composed export failed");
	test.renderer.RequestScreenshot((directory / "inspected.bmp").string());
	test.Frame();
	test.runtime.Command(simxr::control::CommandKind::StrictAdmission, 0, true, 0, 0);
	wait(
		[&](const auto& state)
		{
			return !state.operationPending && state.selectedFrame > exact.selectedFrame && state.selectedReady &&
				   state.selectedLayers == 1 && state.selectedImageCapture == state.selectedCapture;
		},
		"Selected layer view did not produce a retained image");
	const auto raw = test.runtime.Snapshot();
	Require(raw.selectedFrame == exact.selectedFrame + 1, "Strict step admitted more than one frame");
	test.runtime.RequestExport((directory / "layer.ppm").string());
	wait(
		[&](const auto& state)
		{
			return !state.pending && std::filesystem::exists(directory / "layer.ppm");
		},
		"Layer export failed");
	for (const auto capture : test.runtime.CaptureIds())
		test.runtime.Command(simxr::control::CommandKind::Release, capture);
	wait(
		[](const auto& state)
		{
			return !state.operationPending;
		},
		"Capture release failed");
	test.runtime.Command(simxr::control::CommandKind::Resume);
	test.runtime.SetLive(true);
	wait(
		[&](const auto& state)
		{
			return !state.operationPending && !state.manual && state.selectedFrame > raw.selectedFrame;
		},
		"Resume did not restore advancing live output");
	test.renderer.RequestScreenshot((directory / "resumed.bmp").string());
	test.Frame();
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Debugger workflow reported Vulkan validation errors");
	std::ofstream(directory / "frames.txt")
		<< "composed=" << exact.selectedFrame << " layer=" << raw.selectedFrame << " inspected=" << exact.selectedFrame
		<< " resumed=" << test.runtime.Snapshot().selectedFrame << '\n';
	std::cout << "PASS production pause/strict-step/inspect/composed-and-layer-export/resume: frame "
			  << exact.selectedFrame << " -> " << raw.selectedFrame << '\n';
}

void CheckManualEdges(const RuntimeView& target)
{
	RecordingController recording;
	recording.SetTarget({target.endpoint, target.session, target.generation});
	Require(recording.Request(RecordingAction::Start), "Start edge recording");
	Eventually(
		[&]
		{
			const auto s = recording.Snapshot();
			Require(s.error.empty(), s.error.c_str());
			return s.page && s.page->active && !s.busy;
		},
		"Edge recording did not start");
	ManualInputController input;
	input.SetTarget({target.endpoint, target.session, target.generation});
	Eventually(
		[&]
		{
			return input.Snapshot().observation != 0;
		},
		"Edge worker did not connect");
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
		[&]
		{
			const auto s = input.Snapshot();
			Require(s.error.empty(), s.error.c_str());
			return !s.editPending;
		},
		"Button edges did not apply");
	Require(!ReadObservedSetup(input.Snapshot().state).values.input.controllers[1].primary, "Primary remained held");
	Require(recording.Request(RecordingAction::Stop), "Stop edge recording");
	Eventually(
		[&]
		{
			const auto s = recording.Snapshot();
			Require(s.error.empty(), s.error.c_str());
			return s.page && !s.page->active && !s.busy;
		},
		"Edge recording did not stop");
	Require(recording.Request(RecordingAction::Read), "Read recorded edges");
	Eventually(
		[&]
		{
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
	Eventually(
		[&]
		{
			return !input.Snapshot().editPending;
		},
		"Final release did not complete");
	// Losing a transaction must also retire held inputs from this source.
	ManualInputController rival;
	rival.SetTarget({target.endpoint, target.session, target.generation});
	Eventually(
		[&]
		{
			return rival.Snapshot().observation != 0;
		},
		"Rival worker did not connect");
	ManualTransaction headClaim;
	headClaim.commands = {simxr::TakeControl{simxr::ControlChannel::Head, false}};
	Require(rival.Submit(headClaim), "Rival claim refused");
	Eventually(
		[&]
		{
			return !rival.Snapshot().editPending;
		},
		"Rival claim did not apply");
	Require(input.Submit(press), "Held press refused");
	Eventually(
		[&]
		{
			return !input.Snapshot().editPending;
		},
		"Held press did not apply");
	Require(input.Submit(headClaim), "Conflict submission refused");
	Eventually(
		[&]
		{
			return !input.Snapshot().error.empty();
		},
		"Conflict did not reject");
	Eventually(
		[&]
		{
			return !ReadObservedSetup(input.Snapshot().state).values.input.controllers[1].primary;
		},
		"Rejected transaction stranded a held primary button");
	Require(rival.Submit(ReleaseManualInputs{}), "Rival release refused");
	Eventually(
		[&]
		{
			return !rival.Snapshot().editPending;
		},
		"Rival release did not complete");
	std::cout << "PASS native rejection retires held inputs\n";
	std::cout << "PASS native 30 ms tap: press frame " << pressFrame << ", release frame " << releaseFrame << "\n";
}

void CheckNativePlayerRig(const RuntimeView& target)
{
	ManualInputController input;
	input.SetTarget({target.endpoint, target.session, target.generation});
	Eventually(
		[&]
		{
			return input.Snapshot().observation != 0;
		},
		"Rig worker did not connect");
	PlayerRig rig;
	rig.Reset(ReadObservedSetup(input.Snapshot().state));
	const auto submit = [&]
	{
		const auto before = input.Snapshot();
		Require(input.Submit(rig.Transaction(ReadObservedSetup(before.state), before.source)), "Rig edit refused");
		Eventually(
			[&]
			{
				const auto state = input.Snapshot();
				Require(state.error.empty(), state.error.c_str());
				return !state.editPending;
			},
			"Rig edit did not apply");
		return ReadObservedSetup(input.Snapshot().state);
	};
	const auto close = [](XrPosef a, XrPosef b)
	{
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
	Eventually(
		[&]
		{
			return !input.Snapshot().editPending;
		},
		"Rig release did not complete");
	std::cout << "PASS native Player rig: 8 atomic walk/turn frames, 4 World-anchor frames\n";
}

void CheckPreviewPilot(Interaction& test)
{
	const auto target = test.runtime.Snapshot();
	ManualInputController observer;
	observer.SetTarget({target.endpoint, target.session, target.generation});
	const auto pump = [&](int count)
	{
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto observed = [&]
	{
		const auto before = observer.Snapshot().observation;
		observer.Refresh();
		Eventually(
			[&]
			{
				pump(1);
				return observer.Snapshot().observation > before;
			},
			"Pilot observation stalled");
		return ReadObservedSetup(observer.Snapshot().state);
	};
	const auto key = [&](SDL_Scancode code, bool down, SDL_Keymod modifiers = SDL_KMOD_NONE)
	{
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
	const auto button = [&](unsigned code, bool down)
	{
		if (code == SDL_BUTTON_RIGHT && down)
		{
			const auto center = FindPanel("Image area")->InnerRect.GetCenter();
			SDL_Event pointer{};
			pointer.type = SDL_EVENT_MOUSE_MOTION;
			pointer.motion.windowID = SDL_GetWindowID(test.window.Native());
			pointer.motion.x = center.x;
			pointer.motion.y = center.y;
			Require(SDL_PushEvent(&pointer), "Position capture pointer");
		}
		SDL_Event event{};
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.windowID = SDL_GetWindowID(test.window.Native());
		event.button.button = code;
		event.button.down = down;
		Require(SDL_PushEvent(&event), "Push pilot button");
		pump(1);
	};
	const auto motion = [&](float x, float y)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(test.window.Native());
		event.motion.xrel = x;
		event.motion.yrel = y;
		Require(SDL_PushEvent(&event), "Push pilot motion");
		pump(1);
	};
	Require(SDL_ShowWindow(test.window.Native()), "Show isolated pilot window");
	SDL_RaiseWindow(test.window.Native());
	Eventually(
		[&]
		{
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
	button(SDL_BUTTON_RIGHT, true);
	Eventually(
		[&]
		{
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
	const auto handBefore = observed().values.tracking.controllers[1].pose.pose;
	key(SDL_SCANCODE_X, true);
	motion(50, -25);
	pump(5);
	key(SDL_SCANCODE_X, false);
	const auto handAfter = observed().values.tracking.controllers[1].pose.pose;
	Require(std::abs(handAfter.position.x - handBefore.position.x) > .03f, "Preview reach did not move right hand");
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
		[&]
		{
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
	SDL_CloseJoystick(pad);
	Require(SDL_DetachVirtualJoystick(padId), "Detach virtual gamepad");
	pump(10);
	const auto unplugged = observed();
	Require(!unplugged.values.input.controllers[1].primary && unplugged.values.input.controllers[1].trigger == 0 &&
				unplugged.values.input.controllers[0].stick.x == 0,
			"Gamepad unplug left held input");
	std::cout << "PASS native gamepad mapping and unplug release\n";
	Require(test.validation->errors.load() == 0, "Validation during preview piloting");
	std::cout << "PASS native preview walk, look, reach, primary, trigger and Escape release\n";
}

void CheckSpatialEditing(Interaction& test)
{
	using namespace simxr;
	const auto target = test.runtime.Snapshot();
	Require(target.selectedViews.size() == 2, "Displayed image omitted exact compositor views");
	ManualInputController observer;
	observer.SetTarget({target.endpoint, target.session, target.generation});
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
			"Spatial observation stalled");
		return ReadObservedSetup(observer.Snapshot().state);
	};
	const auto pointer = [&](ImVec2 position, int button)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(test.window.Native());
		event.motion.x = position.x;
		event.motion.y = position.y;
		Require(SDL_PushEvent(&event), "Push spatial pointer");
		if (button >= 0)
		{
			event = {};
			event.type = button ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.windowID = SDL_GetWindowID(test.window.Native());
			event.button.button = SDL_BUTTON_LEFT;
			event.button.down = button != 0;
			Require(SDL_PushEvent(&event), "Push spatial button");
		}
		pump(1);
	};
	const auto drag = [&](ImVec2 from, ImVec2 to)
	{
		pointer(from, 1);
		for (int step = 1; step <= 6; ++step)
			pointer({from.x + (to.x - from.x) * step / 6, from.y + (to.y - from.y) * step / 6}, -1);
		pointer(to, 0);
		pump(8);
	};
	SDL_ShowWindow(test.window.Native());
	SDL_RaiseWindow(test.window.Native());
	test.window.Resize(1920, 1080);
	test.workspace.SetScale(1);
	test.workspace.Select(DeviceSel{DevicePage::Right});
	pump(6);
	auto before = observe();
	PlayerRig rig;
	rig.Reset(before);
	auto hand = RelativePose(before.values.tracking.controllers[1].pose.pose, rig.Root());
	auto reach = test.runtime.Snapshot().reachMap;
	Require(reach.width > 100 && reach.height > 100, "Reach map not laid out");
	const ImVec2 center{reach.x + (reach.width - 28) / 2, reach.y + reach.height / 2};
	const float pixels = std::min(reach.height * .44f, (reach.width - 32) * .45f) / .9f;
	drag({center.x + hand.position.x * pixels, center.y + hand.position.z * pixels},
		 {center.x + hand.position.x * pixels, center.y - .30f * pixels});
	auto after = observe();
	hand = RelativePose(after.values.tracking.controllers[1].pose.pose, rig.Root());
	std::cout << "Spatial QA reach rect=" << reach.x << "," << reach.y << "," << reach.width << "," << reach.height
			  << " applied=" << hand.position.x << "," << hand.position.y << "," << hand.position.z << "\n";
	if (std::abs(hand.position.z + .30f) >= .015f)
		if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot((std::filesystem::path(directory) / "spatial-failure.bmp").string());
			pump(1);
			test.renderer.WaitIdle();
		}
	Require(std::abs(hand.position.z + .30f) < .015f, "Reach map did not place the right hand 30 cm forward");
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
	ImGui::ActivateItemByID(ImHashStr("Top", 0, inspector->GetID("Rig view")));
	pump(2);
	const auto view = EulerAngles(0, -1.56f, 0);
	const auto local =
		RelativePose({after.values.tracking.controllers[1].pose.pose.orientation, {}}, {view, {}}).orientation;
	const auto forward = Rotate(local, {0, 0, -1});
	const ImVec2 tip{ball.x + ball.width / 2 + forward.x * ball.width * .46f * .75f,
					 ball.y + ball.height / 2 - forward.y * ball.width * .46f * .75f};
	pointer(tip, 1);
	pointer(tip, 0);
	pump(8);
	after = observe();
	Require(Rotate(after.values.tracking.controllers[1].pose.pose.orientation, {0, 0, -1}).y > .99f,
			"Rotation ball did not point the hand up");
	std::cout << "PASS spatial reach 30 cm and arcball pointing up through native observed state\n";
	hand = RelativePose(after.values.tracking.controllers[1].pose.pose, rig.Root());
	drag({center.x + hand.position.x * pixels, center.y + hand.position.z * pixels},
		 {center.x + 8 * pixels, center.y - .3f * pixels});
	after = observe();
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
	hand = RelativePose(after.values.tracking.controllers[1].pose.pose, rig.Root());
	Require(std::abs(hand.position.x - .2f) < .015f && std::abs(hand.position.z + .55f) < .015f,
			"One-click off-screen recovery did not apply Forward");
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
	ImGui::ActivateItemByID(ImHashStr("Expand", 0, inspector->GetID("Rig view")));
	pump(3);
	Require(test.runtime.Snapshot().rigExpanded, "Rig view did not expand");
	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot((std::filesystem::path(directory) / "spatial-expanded.bmp").string());
		pump(1);
	}
	const auto imageArea = FindPanel("Image area");
	Require(imageArea, "Expanded rig view missing");
	ImGui::ActivateItemByID(ImHashStr("Restore", 0, imageArea->GetID("Rig view")));
	pump(3);
	Require(!test.runtime.Snapshot().rigExpanded, "Rig view did not restore the image");
	std::cout << "PASS expanded rig view and picture-in-picture restore\n";

	if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
	{
		for (float scale : {1.0f, 1.5f})
		{
			test.workspace.SetScale(scale);
			pump(4);
			test.renderer.RequestScreenshot(
				(std::filesystem::path(directory) / (scale == 1 ? "spatial-100.bmp" : "spatial-150.bmp")).string());
			pump(1);
		}
		test.renderer.WaitIdle();
	}
	Require(test.validation->errors.load() == 0, "Spatial preview validation errors");
}

// D8: play without Rec, cut the range into a take, replay it, and compare the
// replay's applied input with the source frames it was cut from.
// Pilots the right hand through real SDL events: about two seconds of trigger
// and primary-button pulses, then Escape.
void DrivePilotPulses(Interaction& test)
{
	const auto pump = [&](int count)
	{
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto push = [&](SDL_Event event)
	{
		Require(SDL_PushEvent(&event), "Push take input");
		pump(1);
	};
	const auto button = [&](unsigned code, bool down)
	{
		SDL_Event event{};
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.windowID = SDL_GetWindowID(test.window.Native());
		event.button.button = code;
		event.button.down = down;
		if (code == SDL_BUTTON_RIGHT && down)
		{
			const auto center = FindPanel("Image area")->InnerRect.GetCenter();
			SDL_Event pointer{};
			pointer.type = SDL_EVENT_MOUSE_MOTION;
			pointer.motion.windowID = event.button.windowID;
			pointer.motion.x = center.x;
			pointer.motion.y = center.y;
			push(pointer);
		}
		push(event);
	};
	const auto key = [&](SDL_Scancode code, bool down)
	{
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
	Eventually([&] { pump(1); return test.window.Focused(); }, "Take window did not receive focus");
	pump(3);
	button(SDL_BUTTON_RIGHT, true);
	Eventually([&] { pump(1); return SDL_GetWindowRelativeMouseMode(test.window.Native()); },
			   "Take pilot did not capture pointer");
	// Pulses of the right trigger and the primary button, about two seconds.
	for (int pulse = 0; pulse < 4; ++pulse)
	{
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
	const auto pump = [&](int count)
	{
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto& history = test.runtime.History();
	const auto begin = history.Size();
	DrivePilotPulses(test);
	Require(history.Size() > begin + 60, "Too few frames while driving input");
	std::size_t pressed = 0;
	for (auto index = begin; index < history.Size(); ++index)
		pressed += history.At(index).hands[1] > 200;
	Require(pressed > 20, "Input lane has no right-hand presses");
	const auto& first = history.At(begin);
	const auto& last = history.At(history.Size() - 1);
	test.workspace.Select(RangeSel{first.identity, last.identity, first.epoch});
	pump(3);
	auto* inspector = FindPanel("Inspector");
	Require(inspector, "Inspector missing for the range page");
	ImGui::ActivateItemByID(inspector->GetID("Save as take"));
	pump(2);
	const auto recordings = directory / "recordings";
	std::filesystem::path take;
	for (const auto& entry : std::filesystem::directory_iterator(recordings))
		if (entry.path().extension() == ".luau")
			take = entry.path();
	Require(!take.empty(), "Save as take wrote no file");
	std::cout << "PASS saved " << history.Size() - begin << " frames as " << take.filename() << '\n';

	const auto replayFrom = history.Size();
	ImGui::ActivateItemByID(inspector->GetID("Replay"));
	pump(2);
	std::optional<std::size_t> anchor;
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(60);
	const auto sourceCount = history.Size() > begin ? replayFrom - begin : 0;
	for (;;)
	{
		pump(1);
		if (!anchor)
			for (auto index = replayFrom; index < history.Size(); ++index)
				if (history.At(index).flags & HistoryDriven)
				{
					anchor = index;
					break;
				}
		if (anchor && history.Size() >= *anchor + sourceCount && !(history.At(history.Size() - 1).flags & HistoryDriven))
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
				ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("Replay details"));
				pump(2);
				test.renderer.RequestScreenshot((std::filesystem::path(qa) / "take-failed.bmp").string());
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
	Require(matched * 10 >= sourceCount * 9, "Replayed input diverges from the source frames");
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot((std::filesystem::path(qa) / "take-replay.bmp").string());
		pump(1);
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors during take replay");
	std::cout << "PASS retroactive take replayed over its source frames\n";
}

// D11: connected layouts at the target sizes and the overlay drawer below 800 effective pixels.
void CheckLayouts(Interaction& test, const std::filesystem::path& qa)
{
	const auto pump = [&](int count)
	{
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto click = [&](ImVec2 point)
	{
		for (const bool down : {true, false})
		{
			SDL_Event event{};
			event.type = SDL_EVENT_MOUSE_MOTION;
			event.motion.windowID = SDL_GetWindowID(test.window.Native());
			event.motion.x = point.x;
			event.motion.y = point.y;
			Require(SDL_PushEvent(&event), "Push layout motion");
			event = {};
			event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
			event.button.windowID = SDL_GetWindowID(test.window.Native());
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
	const std::array<Selection, 4> pages{DeviceSel{DevicePage::Player}, DeviceSel{DevicePage::Right},
										 SessionItemSel{SessionItem::Headset}, LibrarySel{}};
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
			for (const char* name : {"Top bar", "Device rail", "Inspector", "Timeline", "GPU preview"})
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
			test.renderer.RequestScreenshot((qa / name).string());
			pump(2);
		}
		if (overlay)
		{
			const auto& shell = test.workspace.State();
			const auto* inspector = FindPanel("Inspector");
			const auto* preview = FindPanel("GPU preview");
			Require(inspector->Pos.x < preview->Pos.x + preview->Size.x, "Overlay drawer does not cover the preview");
			// The drawer is drawn after the preview, so it covers it.
			Require(inspector->ParentWindow == preview->ParentWindow &&
						inspector->BeginOrderWithinParent > preview->BeginOrderWithinParent,
					"Overlay drawer is drawn under the preview");
			click({preview->Pos.x + 30, preview->Pos.y + preview->Size.y * .6f});
			pump(2);
			Require(!shell.inspectorVisible, "Clicking the preview did not close the drawer");
			test.renderer.RequestScreenshot((qa / "overlay-closed.bmp").string());
			pump(2);
			ImGui::ActivateItemByID(FindPanel("Device rail")->GetID("Right"));
			pump(3);
			Require(shell.inspectorVisible, "Selecting a rail row did not open the drawer");
		}
	}
	test.window.Resize(1280, 800);
	test.workspace.SetScale(1);
	pump(4);
	// Switching eyes, including to Both and back (D12: Both aborted on a style pop).
	for (const char* eye : {"Both", "Right", "Both", "Left"})
	{
		ImGui::ActivateItemByID(FindPanel("GPU preview")->GetID(eye));
		pump(4);
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors in layout capture");
	std::cout << "PASS connected layouts at four sizes and the overlay drawer\n";
}

// D12 feedback: ImGui reports duplicate IDs only for the hovered item, so sweep the pointer
// over every page (the Frame page with its value sections open) and collect conflicts.
void CheckIdConflicts(Interaction& test)
{
	const auto pump = [&](int count)
	{
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(5);
		}
	};
	const auto move = [&](ImVec2 point)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = SDL_GetWindowID(test.window.Native());
		event.motion.x = point.x;
		event.motion.y = point.y;
		Require(SDL_PushEvent(&event), "Push sweep motion");
	};
	std::set<std::string> conflicts;
	const auto sweep = [&](const char* page, const char* region)
	{
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
	const std::array<Selection, 10> pages{DeviceSel{DevicePage::Player},	DeviceSel{DevicePage::Head},
										  DeviceSel{DevicePage::Left},		DeviceSel{DevicePage::Right},
										  DeviceSel{DevicePage::Body},		DeviceSel{DevicePage::Gaze},
										  DeviceSel{DevicePage::Trackers},	SessionItemSel{SessionItem::Headset},
										  SessionItemSel{SessionItem::Scenarios}, LibrarySel{}};
	// Conflicts hide inside sections that start closed, so open every section a page may have.
	const auto openSections = [&]
	{
		for (const char* region : {"Inspector", "Head details", "Hand details", "Configuration details"})
			if (auto* window = FindPanel(region))
				for (const char* header : {"Exact pose", "Connection and profile", "Output", "Advanced connection",
										   "Advanced stepping"})
					window->StateStorage.SetInt(window->GetID(header), 1);
		pump(3);
	};
	for (std::size_t page = 0; page < pages.size(); ++page)
	{
		test.workspace.Select(pages[page]);
		pump(4);
		openSections();
		sweep(("page " + std::to_string(page)).c_str(), "Inspector");
	}
	ImGui::ActivateItemByID(FindPanel("Timeline")->GetID("Pause"));
	Eventually([&] { pump(1); return test.runtime.Snapshot().manual; }, "Sweep pause");
	test.workspace.Select(FrameSel{test.runtime.Snapshot().time.cursor, test.runtime.Snapshot().time.epoch});
	pump(30);
	auto* inspector = FindPanel("Inspector");
	for (const char* header : {"All input values", "Input owners and commands", "All actions and bindings", "Layers",
							   "Timing", "Raw"})
	{
		auto* details = FindPanel("Frame details");
		ImGui::ActivateItemByID((details ? details : inspector)->GetID(header));
		pump(3);
	}
	if (auto* details = FindPanel("Frame details"))
		details->StateStorage.SetInt(details->GetID("Export image"), 1);
	pump(3);
	sweep("frame page", "Inspector");
	if (auto* details = FindPanel("Frame details"))
	{
		ImGui::ActivateItemByID(details->GetID("Open in window"));
		pump(4);
		sweep("raw window", "Raw metadata");
	}
	ImGui::ActivateItemByID(FindPanel("Timeline")->GetID("Continue"));
	pump(4);
	for (const auto& conflict : conflicts)
		std::cout << "ID conflict: " << conflict << '\n';
	Require(conflicts.empty(), "Duplicate ImGui IDs on a page");
	std::cout << "PASS no duplicate IDs under the pointer on any page\n";
}

// D10: record, stop, name, replay and cancel with only Rec, the stop popover and the Library.
void CheckLibrary(Interaction& test, const std::filesystem::path& directory)
{
	const auto pump = [&](int count)
	{
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto screenshot = [&](const char* name)
	{
		if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
		{
			test.renderer.RequestScreenshot((std::filesystem::path(qa) / name).string());
			pump(2);
		}
	};
	test.workspace.Select(DeviceSel{});
	test.runtime.ResumeLive();
	Eventually([&] { pump(1); return test.runtime.Snapshot().time.following && !test.runtime.Snapshot().manual; },
			   "Library check did not start live");
	Eventually([&] { pump(1); return !test.runtime.Snapshot().recordingBusy; }, "Recorder did not finish its first read");
	ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("Rec"));
	Eventually([&] { pump(1); return test.runtime.Snapshot().recording; }, "Rec did not start recording");
	DrivePilotPulses(test);
	Require(test.runtime.Snapshot().recordedChanges > 0, "Piloting recorded no input");
	ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("Stop recording"));
	const auto popover = FindPanel("Top bar")->GetID("Save take");
	Eventually([&] { pump(1); return ImGui::IsPopupOpen(popover, ImGuiPopupFlags_AnyPopupLevel); },
			   "Stop did not open the save popover");
	Eventually([&] { pump(1); return !test.runtime.Snapshot().recording && !test.runtime.Snapshot().recordingBusy; },
			   "Recording did not stop");
	screenshot("library-save.bmp");
	auto* popup = ImGui::GetCurrentContext()->OpenPopupStack.back().Window;
	Require(popup, "Save popover has no window");
	ImGui::ActivateItemByID(popup->GetID("Save"));
	Eventually([&] { pump(1); return std::holds_alternative<TakeSel>(test.workspace.State().selection); },
			   "Saved take was not selected in the Library");
	const auto path = std::get<TakeSel>(test.workspace.State().selection).id;
	Require(std::filesystem::path(path).parent_path() == directory / "recordings" && std::filesystem::exists(path),
			"Saved take is not in the isolated recordings folder");
	pump(3);
	screenshot("library-take.bmp");
	ImGui::ActivateItemByID(FindPanel("Inspector")->GetID("Replay take"));
	Eventually([&] { pump(1); return test.runtime.Snapshot().manual; }, "Library replay did not drive the app");
	pump(10);
	screenshot("library-replay.bmp");
	ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("Cancel replay"));
	Eventually([&] { pump(1); return !test.runtime.Snapshot().manual; }, "Cancel replay did not release the app");
	std::cout << "PASS Rec, stop popover, Library replay and cancel\n";

	// A script from the Library: run, pause, step, resume, finish and report.
	std::filesystem::create_directories(directory / "scripts");
	const auto script = directory / "scripts" / "pose_sweep.luau";
	// examples/rigs/pose_sweep.luau
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
	test.workspace.Select(LibrarySel{});
	pump(3);
	ImGui::ActivateItemByID(FindPanel("Top bar")->GetID("Dismiss replay status"));
	ImGui::ActivateItemByID(FindPanel("Inspector")->GetID("Refresh library"));
	pump(3);
	screenshot("library-list.bmp");
	test.workspace.Select(ScriptSel{script.string()});
	pump(3);
	auto* inspector = FindPanel("Inspector");
	ImGui::ActivateItemByID(inspector->GetID("Run script"));
	Eventually([&] { pump(1); return test.runtime.Snapshot().manual; }, "Script did not start");
	ImGui::ActivateItemByID(inspector->GetID("Pause script"));
	pump(5);
	ImGui::ActivateItemByID(inspector->GetID("Step script"));
	pump(5);
	screenshot("library-script-paused.bmp");
	ImGui::ActivateItemByID(inspector->GetID("Resume script"));
	Eventually([&] { pump(1); return !test.runtime.Snapshot().manual; }, "Script did not finish and release the app");
	pump(5);
	ImGui::ActivateItemByID(inspector->GetID("Save JSON report"));
	pump(2);
	Require(std::filesystem::exists(directory / "scripts" / "pose_sweep.report.json"), "Script report was not saved");
	screenshot("library-script.bmp");
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors in the Library flow");
	std::cout << "PASS Library script run, pause, step, resume and report\n";
}

// D10: every hand input is operable with the mouse alone, through real SDL events.
void CheckInputPad(Interaction& test)
{
	const auto target = test.runtime.Snapshot();
	ManualInputController observer;
	observer.SetTarget({target.endpoint, target.session, target.generation});
	const auto pump = [&](int count)
	{
		for (int index = 0; index < count; ++index)
		{
			test.Frame();
			SDL_Delay(10);
		}
	};
	const auto observed = [&]
	{
		const auto before = observer.Snapshot().observation;
		observer.Refresh();
		Eventually(
			[&]
			{
				pump(1);
				return observer.Snapshot().observation > before;
			},
			"Pad observation stalled");
		return ReadObservedSetup(observer.Snapshot().state).values.input.controllers[1];
	};
	const auto settle = [&](const auto& ready, const char* failure)
	{
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
		for (;;)
		{
			const auto controls = observed();
			if (ready(controls))
				return;
			Require(std::chrono::steady_clock::now() < deadline, failure);
		}
	};
	const auto window = SDL_GetWindowID(test.window.Native());
	const auto move = [&](ImVec2 at)
	{
		SDL_Event event{};
		event.type = SDL_EVENT_MOUSE_MOTION;
		event.motion.windowID = window;
		event.motion.x = at.x;
		event.motion.y = at.y;
		Require(SDL_PushEvent(&event), "Push pad motion");
		pump(1);
	};
	const auto button = [&](unsigned code, bool down)
	{
		SDL_Event event{};
		event.type = down ? SDL_EVENT_MOUSE_BUTTON_DOWN : SDL_EVENT_MOUSE_BUTTON_UP;
		event.button.windowID = window;
		event.button.button = code;
		event.button.down = down;
		Require(SDL_PushEvent(&event), "Push pad button");
		pump(2);
	};
	// The pad lives somewhere inside the inspector; nav focus reports an item's rectangle.
	const auto rect = [&](const char* name)
	{
		auto& g = *ImGui::GetCurrentContext();
		for (auto* candidate : g.Windows)
			if (std::string_view(candidate->Name).find("Inspector") != std::string_view::npos && candidate->Active)
			{
				const auto id = candidate->GetID(name);
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
			test.renderer.RequestScreenshot((std::filesystem::path(qa) / "input-pad-missing.bmp").string());
			pump(1);
			test.renderer.WaitIdle();
		}
		throw std::runtime_error(std::string("Input pad item not found: ") + name);
	};
	Require(SDL_ShowWindow(test.window.Native()), "Show pad window");
	test.workspace.Select(DeviceSel{DevicePage::Right});
	pump(4);
	const auto trigger = rect("Trigger"), grip = rect("Grip"), primary = rect("Primary"),
			   secondary = rect("Secondary"), menu = rect("Menu"), stick = rect("Stick");

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

	move(grip.GetCenter());
	button(SDL_BUTTON_LEFT, true);
	settle([](const auto& c) { return c.grip > .99f; }, "Grip hold did not press");
	button(SDL_BUTTON_LEFT, false);
	settle([](const auto& c) { return c.grip == 0; }, "Grip did not release");
	move(primary.GetCenter());
	button(SDL_BUTTON_LEFT, true);
	settle([](const auto& c) { return c.primary; }, "Primary hold did not press");
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
		test.renderer.RequestScreenshot((std::filesystem::path(qa) / "input-pad.bmp").string());
		pump(1);
	}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors on the input pad");
	std::cout << "PASS pad stick drag, click, spring-back and latch\n";
}

// D9: while the app runs, look back a few seconds and see the kept preview.
void CheckPreviewHistory(Interaction& test)
{
	const auto& history = test.runtime.History();
	const auto& kept = test.runtime.KeptPreviews();
	const auto started = std::chrono::steady_clock::now();
	while (std::chrono::steady_clock::now() - started < std::chrono::seconds(6))
		test.Frame();
	Require(kept.Error().empty(), kept.Error().c_str());
	std::size_t shots = 0;
	for (const auto& shot : kept.Shots())
		shots += shot.frame.frame != 0;
	std::size_t covered = 0;
	for (std::size_t index = 0; index < history.Size(); ++index)
		covered += (history.At(index).flags & HistoryPreview) != 0;
	std::cout << "Kept " << shots << " previews, " << kept.Bytes() / (1 << 20) << " MiB of "
			  << kept.Budget() / (1 << 20) << " MiB; Images lane covers " << covered << " frames\n";
	Require(shots >= 40 && covered >= shots / 2, "Preview history kept too few images");
	Require(kept.Bytes() <= kept.Budget(), "Preview history exceeds its budget");
	const auto index = history.LowerBound(history.At(history.Size() - 1).seconds - 3);
	test.runtime.LookAt(index);
	for (int frame = 0; frame < 4; ++frame)
		test.Frame();
	const auto& frame = history.At(index);
	Require(kept.Nearest(frame.identity, frame.epoch).has_value(), "No kept preview three seconds back");
	Require(!test.runtime.Snapshot().time.following, "Looking back still follows");
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"))
	{
		test.renderer.RequestScreenshot((std::filesystem::path(qa) / "preview-history.bmp").string());
		test.Frame();
	}
	test.runtime.SetLive(true);
	for (int frame = 0; frame < 4; ++frame)
		test.Frame();
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors with preview history");
	std::cout << "PASS kept preview shown three seconds back while running\n";
}

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
		ImGui::ActivateItemByID(timeline->GetID("Next"));
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

void CheckToolbarLaunch(const char* executable)
{
	const auto directoryValue = std::getenv("SIMXR_LAUNCH_QA_DIRECTORY");
	Require(directoryValue != nullptr, "Launch QA requires a new private directory");
	const std::filesystem::path directory(directoryValue);
	Require(directory.is_absolute() && std::filesystem::create_directory(directory),
			"Launch QA directory must be new and absolute");
	const auto settingsPath = directory / "companion.settings";
	Require(::setenv("SIMXR_COMPANION_SETTINGS", settingsPath.c_str(), 1) == 0, "Set isolated settings path");
	CompanionSettings settings;
	LaunchProfile profile;
	profile.name = "Isolated launch QA";
	profile.executable = executable;
	profile.runtimeManifest = std::getenv("XR_RUNTIME_JSON") ? std::getenv("XR_RUNTIME_JSON") : "";
	if (const auto cwd = std::getenv("SIMXR_LAUNCH_QA_CWD"))
		profile.workingDirectory = cwd;
	if (const auto arguments = std::getenv("SIMXR_LAUNCH_QA_ARGUMENTS"))
	{
		std::istringstream lines(arguments);
		for (std::string argument; std::getline(lines, argument);)
			profile.arguments.push_back(argument);
	}
	profile.environment = {{"SIMXR_DATA_DIR", directory.string()}, {"SIMXR_CONTROL_SOCKET", ""}};
	settings.profiles.push_back(profile);
	settings.activeProfile = profile.name;
	SaveSettings(settingsPath, settings);
	Interaction test(true);
	for (unsigned frame = 0; frame < 4; ++frame)
		test.Frame();
	test.renderer.RequestScreenshot((directory / "before-launch.bmp").string());
	test.Frame();
	const auto* toolbar = FindPanel("##Workspace toolbar");
	Require(toolbar != nullptr, "Launch toolbar absent");
	test.Click({toolbar->Pos.x + 20, toolbar->Pos.y + ImGui::GetFrameHeight() / 2});
	const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
	Require(!popups.empty() && popups.back().Window, "App picker did not open");
	ImGui::ActivateItemByID(popups.back().Window->GetID(profile.name.c_str()));
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
	Require(std::filesystem::path(state.endpoint).parent_path() == directory, "Launch selected unrelated endpoint");
	std::ofstream(directory / "attached.endpoint") << state.endpoint;
	if (std::getenv("SIMXR_TIMELINE_QA"))
		CheckTimelineWorkflow(test);
	if (std::getenv("SIMXR_PAD_QA"))
		CheckInputPad(test);
	if (std::getenv("SIMXR_HISTORY_QA"))
		CheckPreviewHistory(test);
	if (std::getenv("SIMXR_TAKE_QA"))
		CheckInputTake(test, directory);
	if (std::getenv("SIMXR_LIBRARY_QA"))
		CheckLibrary(test, directory);
	if (const auto qa = std::getenv("SIMXR_UI_QA_DIR"); qa && std::getenv("SIMXR_LAYOUT_QA"))
		CheckLayouts(test, qa);
	if (std::getenv("SIMXR_ID_QA"))
		CheckIdConflicts(test);
	if (std::getenv("SIMXR_INPUT_QA"))
		CheckManualEdges(state);
	if (std::getenv("SIMXR_RIG_QA"))
		CheckNativePlayerRig(state);
	if (std::getenv("SIMXR_PILOT_QA"))
		CheckPreviewPilot(test);
	if (std::getenv("SIMXR_SPATIAL_QA"))
		CheckSpatialEditing(test);
	if (std::getenv("SIMXR_RETENTION_QA"))
		CheckSnapshotRetention(test);
	Eventually([&] { test.Frame(); return test.runtime.History().Size() > 0; },
		"Launched runtime did not populate timeline history");
	Require(test.runtime.Snapshot().error.empty(), "Launched runtime reported a control error");
	test.renderer.RequestScreenshot((directory / "toolbar-launch.bmp").string());
	test.Frame();
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors in launched preview");
	std::cout << "PASS toolbar launched and attached stereo runtime " << state.endpoint << '\n';
}

#endif

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
			for (int page = 0; page < 10; ++page)
			{
				if (page < 7)
					test.workspace.Select(DeviceSel{static_cast<DevicePage>(page)});
				else if (page < 9)
					test.workspace.Select(SessionItemSel{static_cast<SessionItem>(page - 7)});
				else
					test.workspace.Select(LibrarySel{});
				for (int frame = 0; frame < 4; ++frame)
					test.Frame();
				for (const char* name : {"Top bar", "Device rail", "Inspector", "Timeline", "GPU preview"})
				{
					const auto* window = FindPanel(name);
					Require(window && !window->ScrollbarX, "Shell region has horizontal scrolling");
					Require(window->Pos.x >= 0 && window->Pos.x + window->Size.x <= size.x + 1,
							"Shell region extends beyond window");
				}
				const auto path =
					directory / (std::to_string(static_cast<int>(size.x)) + "-" +
								 std::to_string(static_cast<int>(scale * 100)) + "-" + std::to_string(page) + ".bmp");
				test.renderer.RequestScreenshot(path.string());
				test.Frame();
			}
		}
	test.renderer.WaitIdle();
	Require(test.validation->errors.load() == 0, "Validation errors during layout capture");
}

} // namespace

int main()
{
	try
	{

#ifdef __linux__
		if (const auto endpoint = std::getenv("SIMXR_TIMELINE_ENDPOINT"))
		{
			Interaction test;
			test.live = true;
			test.runtime.ConnectEndpoint(endpoint);
			Eventually([&] { test.Frame(); return test.runtime.Snapshot().selectedReady; }, "Timeline app preview absent");
			CheckTimelineWorkflow(test);
			return 0;
		}
		if (const auto endpoint = std::getenv("SIMXR_PREVIEW_PILOT_ENDPOINT"))
		{
			Interaction test;
			test.live = true;
			test.runtime.ConnectEndpoint(endpoint);
			Eventually(
				[&]
				{
					test.Frame();
					return test.runtime.Snapshot().selectedReady;
				},
				"Existing isolated app did not supply a preview");
			if (std::getenv("SIMXR_SPATIAL_QA"))
				CheckSpatialEditing(test);
			else
				CheckPreviewPilot(test);
			if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
			{
				test.renderer.RequestScreenshot((std::filesystem::path(directory) / "cemu-gameplay.bmp").string());
				test.Frame();
				test.renderer.WaitIdle();
			}
			return 0;
		}
		if (const auto endpoint = std::getenv("SIMXR_DEBUG_QA_ENDPOINT"))
		{
			CheckDebuggerWorkflow(endpoint);
			return 0;
		}
		if (const auto executable = std::getenv("SIMXR_LAUNCH_QA_EXECUTABLE"))
		{
			CheckToolbarLaunch(executable);
			return 0;
		}
#endif
		auto validation = std::make_shared<ValidationLog>();
		{
			Interaction test;
			validation = test.validation;
			for (unsigned frame = 0; frame < 4; ++frame)
				test.Frame();
			test.SelectWorkspace(LibrarySel{});
			test.CheckControls();
			test.SelectWorkspace(DeviceSel{});
			test.SelectWorkspace(FrameSel{});
			test.SelectWorkspace(LibrarySel{});
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
			CheckTimelineCanvas(test);
			test.CheckFocusLoss();
			test.renderer.WaitIdle();
		}
		Require(validation->errors.load() == 0, "Vulkan validation errors during interaction or teardown");
		CheckShell();
		ConfigureObservedDevices();
		if (const auto endpoint = std::getenv("SIMXR_UI_PILOT_ENDPOINT"))
			PilotLiveRuntime(endpoint);
		if (const auto directory = std::getenv("SIMXR_UI_QA_DIR"))
			CaptureLayouts(directory);
		std::cout << "PASS fixed shell, rail selection and SDL input across three resizes\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
