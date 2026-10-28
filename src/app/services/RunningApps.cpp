#include "app/services/RunningApps.h"
#include "Pch.h"
#include "app/platform/RunningApps.h"
#include "app/services/RuntimeEndpoints.h"

namespace simxr::app
{
std::vector<RunningApp> DiscoverRunningApps(std::chrono::milliseconds timeout, std::stop_token stop)
{
	std::vector<RunningApp> apps;
	for (auto& endpoint : DiscoverRuntimeEndpoints())
	{
		if (stop.stop_requested())
			break;
		auto app = ProbeRunningApp(endpoint, timeout);
		if (!app)
			continue;
		if (app->name.empty())
		{
			const auto slash = endpoint.find_last_of("/\\");
			app->name = endpoint.substr(slash == std::string::npos ? 0 : slash + 1);
		}
		app->endpoint = std::move(endpoint);
		apps.push_back(std::move(*app));
	}
	return apps;
}
std::optional<LaunchProfile> DescribeProcess(std::uint64_t process)
{
	auto profile = ReadProcessLaunch(process);
	if (!profile || profile->executable.empty() || profile->workingDirectory.empty())
		return {};
	try
	{
		ValidateLaunchProfile(*profile);
	}
	catch (const std::invalid_argument&)
	{
		return {};
	}
	return profile;
}
} // namespace simxr::app
