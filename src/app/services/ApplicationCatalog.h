#pragma once

#include "app/services/LaunchTypes.h"
#include "app/services/AutomationOwner.h"
#include "automation/RigTypes.h"

#include <cstdint>
#include <memory>
#include <stdexcept>
#include <string>
#include <vector>

namespace simxr::app
{
struct ApplicationSnapshot
{
	std::uint64_t id = 0, session = 0, generation = 0, runtimeProcess = 0, runtimeEpoch = 0;
	LaunchSnapshot launch;
	std::string name, endpoint, error;
	bool connected = false, running = false;
	std::string launchEndpoint;
	std::string launchDataDirectory;
};
struct LuauRunRequest
{
	std::string source, name;
	std::uint64_t seed = 0;
	int deadlineSeconds = 30, memoryMiB = 64;
};
struct LuauRunStatus
{
	std::uint64_t run = 0;
	automation::RigSnapshot snapshot;
};
enum class LuauControl
{
	Pause,
	Resume,
	Step,
	Cancel
};

// Application-thread access to the same launcher and supervision services in either host.
class ApplicationCatalog
{
  public:
	virtual ~ApplicationCatalog() = default;
	virtual std::uint64_t LaunchApplication(const LaunchProfile& profile) = 0;
	virtual void RequestApplicationClose(std::uint64_t id) = 0;
	virtual void RequestApplicationRestart(std::uint64_t id, std::uint64_t expectedProcess) = 0;
	virtual std::vector<ApplicationSnapshot> Applications() const = 0;
	virtual bool RequestShutdown() = 0;
	virtual std::uint64_t StartLuau(std::uint64_t, LuauRunRequest, const std::shared_ptr<AutomationOwner>&)
	{
		throw std::runtime_error("Luau automation is unavailable in this build");
	}
	virtual LuauRunStatus LuauStatus(std::uint64_t, std::uint64_t, const std::shared_ptr<AutomationOwner>&) const
	{
		throw std::runtime_error("Luau automation is unavailable in this build");
	}
	virtual void ControlLuau(std::uint64_t, std::uint64_t, LuauControl, const std::shared_ptr<AutomationOwner>&)
	{
		throw std::runtime_error("Luau automation is unavailable in this build");
	}
};
} // namespace simxr::app
