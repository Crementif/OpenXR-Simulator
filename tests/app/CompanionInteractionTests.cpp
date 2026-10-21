#include "../TestSupport.h"
#include "app/ProcessSupervisor.h"
#include "app/RuntimePanels.h"
#include "app/UiRenderer.h"
#include "app/Window.h"
#include "app/WorkspacePanels.h"

#include <SDL3/SDL.h>
#include <cstdlib>
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
	RuntimePanels runtime{renderer, ""};
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
		const char* labels[] = {"Simulator", "Debug", "Automation"};
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
			for (unsigned frame = 0; frame < 12; ++frame)
			{
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
				if (frame == 11)
					test.renderer.RequestScreenshot(path.string());
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
