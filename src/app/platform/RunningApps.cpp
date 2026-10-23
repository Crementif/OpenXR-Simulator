#include "app/platform/RunningApps.h"
#include "Pch.h"

#include <stdexcept>

namespace simxr::app
{
std::optional<RunningApp> ProbeRunningApp(const std::string&, std::chrono::milliseconds)
{
	throw std::runtime_error("Running application discovery is unavailable on this platform");
}
std::optional<LaunchProfile> ReadProcessLaunch(std::uint64_t)
{
	return {};
}
} // namespace simxr::app
