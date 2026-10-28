#pragma once

#include "app/services/RunningApps.h"

#include <imgui.h>

#include <cstddef>
#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace simxr::app
{
struct CompanionSettings;
struct RuntimeView;
struct AppPickerChoice
{
	std::optional<RunningApp> connect;
	std::optional<int> launchProfile;
	std::optional<LaunchProfile> launchRecent;
	std::optional<std::size_t> saveRecent;
	std::string discoveryError;
	bool addProfile = false, editProfiles = false, connectEndpoint = false, disconnect = false;
};
class AppPicker
{
  public:
	~AppPicker();
	AppPicker() = default;
	AppPicker(const AppPicker&) = delete;
	AppPicker& operator=(const AppPicker&) = delete;
	void StopDiscovery() noexcept;
	bool DiscoveryStopped() const noexcept;
	AppPickerChoice Draw(const CompanionSettings& settings, const RuntimeView& view, const std::string& label,
						 ImU32 dot, const std::string& tooltip, bool open, const std::function<void()>& session,
						 float maximumWidth = 0, bool compact = false);

  private:
	std::string Refresh();
	void Run(std::stop_token stop) noexcept;
	struct DiscoveryResult
	{
		std::vector<RunningApp> apps;
		std::string error;
	};
	std::mutex m_mutex;
	std::condition_variable_any m_changed;
	std::jthread m_worker;
	std::atomic<bool> m_workerFailed{false};
	std::atomic<bool> m_workerExited{true};
	std::uint64_t m_requested = 0;
	std::optional<DiscoveryResult> m_result;
	bool m_discoveryFailed = false, m_loading = false;
	std::vector<RunningApp> m_running;
};
} // namespace simxr::app
