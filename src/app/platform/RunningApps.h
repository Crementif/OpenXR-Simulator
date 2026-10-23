#pragma once

#include "app/services/RunningApps.h"

namespace simxr::app
{
std::optional<RunningApp> ProbeRunningApp(const std::string& endpoint, std::chrono::milliseconds timeout);
std::optional<LaunchProfile> ReadProcessLaunch(std::uint64_t process);
} // namespace simxr::app
