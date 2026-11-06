#include "app/services/RunningApps.h"
#include "Pch.h"
#include "app/platform/RunningApps.h"
#include "app/services/RuntimeEndpoints.h"

namespace simxr::app
{
static std::string EndpointName(const std::string& endpoint)
{
	const auto slash = endpoint.find_last_of("/\\");
	return endpoint.substr(slash == std::string::npos ? 0 : slash + 1);
}

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
			app->name = EndpointName(endpoint);
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

RunningAppWatcher::RunningAppWatcher() : m_worker([this](std::stop_token stop) { Run(stop); }) {}

std::vector<RunningApp> RunningAppWatcher::TakeAppeared()
{
	std::lock_guard lock(m_mutex);
	return std::exchange(m_appeared, {});
}

void RunningAppWatcher::Run(std::stop_token stop) noexcept
{
	constexpr int maximumAttempts = 5;
	try
	{
		std::set<std::string> known;
		// Failed probes per endpoint; a stale endpoint stops being probed after a few.
		std::map<std::string, int> pending;
		for (auto& endpoint : DiscoverRuntimeEndpoints())
			known.insert(std::move(endpoint));
		while (true)
		{
			{
				std::unique_lock lock(m_mutex);
				m_wake.wait_for(lock, stop, std::chrono::seconds(1), [] { return false; });
				if (stop.stop_requested())
					return;
			}
			const auto endpoints = DiscoverRuntimeEndpoints();
			const std::set<std::string> present(endpoints.begin(), endpoints.end());
			std::erase_if(known, [&](const std::string& endpoint) { return !present.contains(endpoint); });
			std::erase_if(pending, [&](const auto& entry) { return !present.contains(entry.first); });
			for (const auto& endpoint : endpoints)
			{
				if (stop.stop_requested())
					return;
				if (known.contains(endpoint))
					continue;
				auto app = ProbeRunningApp(endpoint, std::chrono::milliseconds(150));
				if (!app)
				{
					if (++pending[endpoint] >= maximumAttempts)
						known.insert(endpoint);
					continue;
				}
				known.insert(endpoint);
				pending.erase(endpoint);
				if (app->name.empty())
					app->name = EndpointName(endpoint);
				app->endpoint = endpoint;
				std::lock_guard lock(m_mutex);
				m_appeared.push_back(std::move(*app));
			}
		}
	}
	catch (...)
	{
		// Discovery failures only cost the automatic tabs; the picker reports its own errors.
	}
}
} // namespace simxr::app
