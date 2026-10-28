#include "Pch.h"
#include "app/services/LuauAutomation.h"
#include "app/services/RigController.h"

#include <stdexcept>

namespace simxr::app
{
std::uint64_t StartLuauRun(RigController& rigs, const ApplicationSnapshot& application, LuauRunRequest request,
						   const std::shared_ptr<AutomationOwner>& owner)
{
	if (!application.connected || !application.running || application.endpoint.empty() || !application.session ||
		!application.generation || !application.runtimeProcess || application.launch.restartPending)
		throw std::runtime_error("Select an application with a connected running XR session");
	if (request.source.empty() || request.source.size() > 512 * 1024 || request.source.find('\0') != std::string::npos ||
		request.name.empty() || request.name.size() > 256 || request.name.find('\0') != std::string::npos ||
		request.deadlineSeconds < 1 || request.deadlineSeconds > 3600 || request.memoryMiB < 1 || request.memoryMiB > 512)
		throw std::invalid_argument("Invalid bounded Luau source or execution options");
	return rigs.RunSource({application.endpoint, application.session, application.generation}, std::move(request.source),
		std::move(request.name), {request.seed, request.deadlineSeconds, request.memoryMiB}, owner,
		application.runtimeProcess);
}
LuauRunStatus ReadLuauRun(const RigController& rigs, std::uint64_t run, const std::shared_ptr<AutomationOwner>& owner)
{
	if (!run)
		run = rigs.RunId();
	if (!rigs.OwnedBy(owner, run))
		throw std::runtime_error("Luau run is absent or belongs to another MCP session");
	return {run, rigs.Snapshot()};
}
void ControlLuauRun(RigController& rigs, std::uint64_t run, LuauControl action,
					const std::shared_ptr<AutomationOwner>& owner)
{
	if (!rigs.OwnedBy(owner, run))
		throw std::runtime_error("Luau run is absent or belongs to another MCP session");
	if (!rigs.IsActive())
		throw std::runtime_error("Luau run has already finished");
	switch (action)
	{
	case LuauControl::Pause:
		rigs.Pause();
		break;
	case LuauControl::Resume:
		rigs.Resume();
		break;
	case LuauControl::Step:
		rigs.Step();
		break;
	case LuauControl::Cancel:
		rigs.Cancel();
		break;
	}
}
} // namespace simxr::app
