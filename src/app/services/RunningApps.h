#pragma once

#include "app/services/LaunchTypes.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace simxr::app
{
// A runtime control endpoint that accepted a connection just now.
struct RunningApp
{
	std::string endpoint;
	std::string name;
	std::string executable;
	std::uint64_t process = 0;
	std::optional<std::chrono::system_clock::time_point> started;
};
// Lists the runtimes in the control socket directory, including ones no
// companion is attached to. Each endpoint gets a bare connect probe with the
// timeout: nothing is sent, so discovery never takes control, and sockets
// left behind by ended processes are skipped.
std::vector<RunningApp> DiscoverRunningApps(std::chrono::milliseconds timeout = std::chrono::milliseconds(150));
// The executable, arguments, working folder and environment needed to relaunch.
// Unavailable when process details cannot be read or represented without truncation.
std::optional<LaunchProfile> DescribeProcess(std::uint64_t process);
} // namespace simxr::app
