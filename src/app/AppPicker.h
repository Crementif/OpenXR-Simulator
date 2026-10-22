#pragma once

#include "app/CompanionSettings.h"
#include "app/RunningApps.h"

#include <chrono>
#include <optional>
#include <string>
#include <vector>

namespace simxr::app
{
struct RuntimeView;
// What the user chose in the app picker; the shell carries it out.
struct AppPickerChoice
{
	std::optional<RunningApp> connect;
	std::optional<int> launchProfile;
	std::optional<LaunchProfile> launchRecent;
	std::optional<std::size_t> saveRecent;
	bool addProfile = false, editProfiles = false, connectEndpoint = false, disconnect = false;
};
// The top bar's app button and its menu: runtimes running now, recent apps and
// launch profiles.
class AppPicker
{
  public:
	AppPickerChoice Draw(const CompanionSettings& settings, const RuntimeView& view, const std::string& label,
						 bool open);

  private:
	void Refresh();
	std::vector<RunningApp> m_running;
	std::chrono::steady_clock::time_point m_nextScan;
};
} // namespace simxr::app
