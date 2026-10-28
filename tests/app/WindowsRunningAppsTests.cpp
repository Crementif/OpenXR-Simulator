#include "../TestSupport.h"
#include "app/services/RunningApps.h"
#include "app/platform/windows/RuntimeEndpoints.h"
#include "app/platform/windows/LaunchSupport.h"
#include <algorithm>
#include <iostream>

int main()
{
	using namespace simxr::app;
	using namespace std::chrono_literals;
	try
	{
		const auto endpoint = std::string(R"(\\.\pipe\SimXR.Control.DiscoveryTest.)") + std::to_string(GetCurrentProcessId());
		const auto path = windows::Wide(endpoint);
		windows::Handle pipe(CreateNamedPipeW(path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1024, 1024, 0, nullptr));
		Require(pipe.Get() != INVALID_HANDLE_VALUE, "Create discovery pipe");
		Require(SetEnvironmentVariableW(L"SIMXR_CONTROL_SOCKET", path.c_str()), "Select fixture endpoint");
		const auto apps = DiscoverRunningApps(25ms);
		const auto found = std::find_if(apps.begin(), apps.end(), [&](const auto& app) { return app.endpoint == endpoint; });
		Require(found != apps.end() && found->process == GetCurrentProcessId(), "Probe lost the server PID");
		Require(!found->name.empty() && !found->executable.empty() && found->started.has_value(), "Missing process details");
		const auto start = std::chrono::steady_clock::now();
		const auto busy = DiscoverRunningApps(25ms);
		Require(std::chrono::steady_clock::now() - start < 2s, "Busy pipe probe exceeded its bound");
		Require(std::none_of(busy.begin(), busy.end(), [&](const auto& app) { return app.endpoint == endpoint; }),
			"Busy disconnected pipe advertised as connectable");
		pipe = windows::Handle{};
		Require(!RuntimeEndpointExists(endpoint), "Ended pipe still exists");
		const auto ended = DiscoverRunningApps(25ms);
		Require(std::none_of(ended.begin(), ended.end(), [&](const auto& app) { return app.endpoint == endpoint; }),
			"Ended pipe remained in Running");
		std::cout << "PASS Windows process details, bounded probe and ended endpoint\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
