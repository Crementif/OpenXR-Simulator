#pragma once

#include "app/services/ApplicationCatalog.h"

#include <memory>
#include <vector>

namespace simxr::app
{
class ProcessSupervisor;

// Owns the same process launcher and runtime discovery workers used by the UI.
class HeadlessApplications final : public ApplicationCatalog
{
public:
	explicit HeadlessApplications(ProcessSupervisor& processes);
	~HeadlessApplications();
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
	bool ShutdownRequested() const noexcept;
	void Poll();

private:
	struct Entry;
	ProcessSupervisor& m_processes;
	std::vector<std::unique_ptr<Entry>> m_applications;
	std::uint64_t m_nextId = 1;
	bool m_shutdown = false;
};
} // namespace simxr::app
