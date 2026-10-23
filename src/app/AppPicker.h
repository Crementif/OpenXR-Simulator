#pragma once

#include "app/CompanionSettings.h"
#include "app/RunningApps.h"

#include <imgui.h>

#include <chrono>
#include <functional>
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
// The top bar's app control: icon, app name and run-state dot, flat until hovered.
// Its menu holds the connected app's session actions, then runtimes running now,
// recent apps and launch profiles.
class AppPicker
{
  public:
	AppPickerChoice Draw(const CompanionSettings& settings, const RuntimeView& view, const std::string& label,
						 ImU32 dot, const std::string& tooltip, bool open, const std::function<void()>& session);

  private:
	void Refresh();
	std::vector<RunningApp> m_running;
	std::chrono::steady_clock::time_point m_nextScan;
};
} // namespace simxr::app
