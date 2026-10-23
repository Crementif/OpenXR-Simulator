#pragma once

#include "app/services/CompanionSettings.h"

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
// How the process behind a running app was started, as far as the system shows
// it: executable, arguments, working folder, runtime JSON and process environment.
std::optional<LaunchProfile> DescribeProcess(std::uint64_t process);
} // namespace simxr::app
