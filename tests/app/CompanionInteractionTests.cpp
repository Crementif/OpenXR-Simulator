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
#include <fstream>
#include <sstream>

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
	WorkspacePanels workspace;
	RuntimePanels runtime{renderer, ""};
	ImVec2 buttonCenter;
	ImVec2 inputCenter;
	char input[32]{};
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
			runtime.Poll();
		workspace.BeginFrame(runtime);
		if (live)
		{
			workspace.DrawDockspace();
			workspace.Draw(runtime);
			runtime.Draw(workspace.Mode());
			Require(renderer.Render(runtime.ReadyWaits()), "Live launch UI frame was not rendered");
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
		Require(renderer.Render(runtime.ReadyWaits()), "Admitted UI frame was not rendered");
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

#ifdef __linux__
void CheckToolbarLaunch(const char* executable)
{
	const auto directoryValue = std::getenv("SIMXR_LAUNCH_QA_DIRECTORY");
	Require(directoryValue != nullptr, "Launch QA requires a new private directory");
	const std::filesystem::path directory(directoryValue);
	Require(directory.is_absolute() && std::filesystem::create_directory(directory), "Launch QA directory must be new and absolute");
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
	const auto* toolbar = ImGui::FindWindowByName("##Workspace toolbar");
	Require(toolbar != nullptr, "Launch toolbar absent");
	const auto& style = ImGui::GetStyle();
	const auto width = std::max(100.0f, std::min(220.0f, ImGui::GetMainViewport()->WorkSize.x * 0.30f));
	test.Click({toolbar->Pos.x + style.WindowPadding.x + width + style.ItemSpacing.x + 12,
		toolbar->Pos.y + style.WindowPadding.y + ImGui::GetFrameHeightWithSpacing() + ImGui::GetFrameHeight() / 2});
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(20);
	while (!test.runtime.Snapshot().selectedReady)
	{
		test.Frame();
		Require(std::chrono::steady_clock::now() < deadline, "Toolbar launch did not attach and display live pixels");
	}
	const auto state = test.runtime.Snapshot();
	Require(state.connected && state.sessionAvailable && state.selectedLayers == 2, "Launched stereo session not selected");
	Require(std::filesystem::path(state.endpoint).parent_path() == directory, "Launch selected unrelated endpoint");
	std::ofstream(directory / "attached.endpoint") << state.endpoint;
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

#ifdef __linux__
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
