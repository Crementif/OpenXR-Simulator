#pragma once

#include "app/platform/Window.h"
#include "app/render/UiRenderer.h"
#include "app/services/ProcessSupervisor.h"
#include "app/WorkspacePanels.h"
#include "app/RuntimePanels.h"
#include "../../TestSupport.h"

#include <filesystem>
#include <functional>
#include <memory>
#include <imgui_internal.h>

#ifdef _WIN32
extern "C" bool SDL_SetKeyboardFocus(SDL_Window* window);
extern "C" void SDL_SetMouseFocus(SDL_Window* window);
#endif

namespace simxr::tests
{
using namespace app;

inline constexpr SDL_MouseID TestMouse = 0x51A7;
void IgnoreRealPointer();
ImGuiWindow* FindPanel(const char* name);
bool UseValidation();
std::filesystem::path QaDirectory(const char* name);

template<typename Pump>
void ChooseRigView(ImGuiID rig, const char* view, Pump pump)
{
	ImGui::ActivateItemByID(ImHashStr("##View", 0, rig));
	pump(2);
	const auto& popups = ImGui::GetCurrentContext()->OpenPopupStack;
	Require(!popups.empty() && popups.back().Window, "Rig view menu did not open");
	ImGui::ActivateItemByID(popups.back().Window->GetID(view));
	pump(2);
}

class Interaction
{
public:
	Window window{true};
	std::shared_ptr<ValidationLog> validation = std::make_shared<ValidationLog>();
	UiRenderer renderer{window, UseValidation(), validation, true};
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

	bool PollEvents();
	void Frame();
	void Click(ImVec2 position);
	void SelectWorkspace(Selection selection);
	void CheckFocusLoss();
	void CheckControls();
};
}
