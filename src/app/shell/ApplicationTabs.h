#pragma once

#include "app/services/ApplicationCatalog.h"
#include "app/services/RunningApps.h"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <variant>
#include <vector>
#include <vulkan/vulkan.h>

namespace simxr::app
{
class ProcessSupervisor;
class RuntimePanels;
class UiRenderer;
class Window;
class WorkspacePanels;

class ApplicationTabs : public ApplicationCatalog
{
public:
	ApplicationTabs(ProcessSupervisor& processes, UiRenderer& renderer, Window& window, bool enabled,
					std::string endpoint, bool showTabs);
	~ApplicationTabs();

	RuntimePanels& ActiveRuntime();
	WorkspacePanels& ActiveWorkspace();
	std::size_t Count() const noexcept;
	void OpenApplication(RunningApp app);
	bool PilotActive() const;
	void FocusLost();
	void Draw();
	std::vector<VkSemaphore> ReadyWaits() const;
	void RecordTransfers(VkCommandBuffer command);
	void Submitted(std::uint64_t serial);
	void Finish();

	std::uint64_t LaunchApplication(const LaunchProfile& profile) override;
	void RequestApplicationClose(std::uint64_t id) override;
	void RequestApplicationRestart(std::uint64_t id, std::uint64_t expectedProcess) override;
	std::vector<ApplicationSnapshot> Applications() const override;
	bool RequestShutdown() override;
#ifdef SIMXR_COMPANION_AUTOMATION
	std::uint64_t StartLuau(std::uint64_t id, LuauRunRequest request,
							const std::shared_ptr<AutomationOwner>& owner) override;
	LuauRunStatus LuauStatus(std::uint64_t id, std::uint64_t run,
							 const std::shared_ptr<AutomationOwner>& owner) const override;
	void ControlLuau(std::uint64_t id, std::uint64_t run, LuauControl action,
					 const std::shared_ptr<AutomationOwner>& owner) override;
#endif

private:
	struct Tab;
	using OpenRequest = std::variant<RunningApp, LaunchProfile>;
	Tab& Add(std::string endpoint = {});
	Tab* Find(std::uint64_t id) const;
	void ApplyRequests();
	void DrawBar();
	void BeginClose(Tab& tab);
	ProcessSupervisor& m_processes;
	UiRenderer& m_renderer;
	Window& m_window;
	bool m_enabled, m_showTabs, m_forceSelection = false, m_headsetUncertain = false;
	std::vector<std::unique_ptr<Tab>> m_tabs;
	std::uint64_t m_active = 0, m_nextId = 1;
	std::optional<std::uint64_t> m_select, m_confirmClose;
	std::optional<OpenRequest> m_open;
	bool m_add = false;
};
} // namespace simxr::app
