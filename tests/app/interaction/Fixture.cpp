#include "app/UiPch.h"
#include "Fixture.h"
#include "app/services/Utf8Path.h"
#ifdef _WIN32
#include "app/platform/windows/LaunchSupport.h"
#endif
#include <iostream>

namespace simxr::tests
{
void IgnoreRealPointer()
{
	SDL_SetEventFilter(
		[](void*, SDL_Event* event) {
			switch (event->type)
			{
			case SDL_EVENT_MOUSE_MOTION:
				return event->motion.which == TestMouse;
			case SDL_EVENT_MOUSE_BUTTON_DOWN:
			case SDL_EVENT_MOUSE_BUTTON_UP:
				return event->button.which == TestMouse;
			case SDL_EVENT_MOUSE_WHEEL:
				return event->wheel.which == TestMouse;
			default:
				return true;
			}
		},
		nullptr);
}

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

bool UseValidation()
{
	static const bool enabled = [] {
		const auto mode = std::getenv("SIMXR_UI_VALIDATION");
		Require(!mode || std::strcmp(mode, "0") == 0 || std::strcmp(mode, "1") == 0,
				"SIMXR_UI_VALIDATION must be 0 or 1");
		const bool requested = !mode || std::strcmp(mode, "0") != 0;
		std::cout << (requested ? "Vulkan validation=required\n"
								: "Vulkan validation=disabled explicitly; functional checks only, GPU validation "
								  "remains unqualified\n")
				  << std::flush;
		return requested;
	}();
	return enabled;
}

std::filesystem::path QaDirectory(const char* name)
{
#ifdef _WIN32
	return Utf8Path(windows::Environment(name));
#else
	const auto value = std::getenv(name);
	return value ? Utf8Path(value) : std::filesystem::path{};
#endif
}

bool Interaction::PollEvents()
{
	return window.PollEvents([&](const SDL_Event& event) { renderer.ProcessEvent(event); });
}

void Interaction::Frame()
{
	Eventually(
		[&] {
			Require(PollEvents(), "Unexpected window closure");
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
		if (extraDraw)
			extraDraw();
		const auto transfers = [&](VkCommandBuffer command) {
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
	if (extraDraw)
		extraDraw();
	const auto transfers = [&](VkCommandBuffer command) {
		runtime.RecordTransfers(command);
	};
	Require(renderer.Render(runtime.ReadyWaits(), transfers), "Admitted UI frame was not rendered");
	runtime.Submitted(renderer.SubmittedFrames());
}

void Interaction::Click(ImVec2 position)
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

void Interaction::SelectWorkspace(Selection selection)
{
	workspace.Select(std::move(selection));
	Frame();
	Require(FindPanel("Inspector") != nullptr, "Inspector is missing");
}

void Interaction::CheckFocusLoss()
{
	window.TakeFocusLost();
	SDL_Event event{};
	event.type = SDL_EVENT_WINDOW_FOCUS_LOST;
	event.window.windowID = SDL_GetWindowID(window.Native());
	Require(SDL_PushEvent(&event), "Cannot enqueue focus loss");
	Require(PollEvents() && window.TakeFocusLost(), "Focus loss was not delivered to the application");
	Require(!window.TakeFocusLost(), "Focus loss must be consumed once");
}

void Interaction::CheckControls()
{
	workspace.Select(DeviceSel{});
	Frame();
	const auto before = clicks;
	Click(buttonCenter);
	Require(clicks == before + 1, "Other controls stopped responding after selection");
	workspace.ShowLibrary(true);
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
}
