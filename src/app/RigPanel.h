#pragma once

#include "app/CompanionSettings.h"
#include "app/LaunchTypes.h"

#include <cstdint>
#include <memory>
#include <string>

namespace simxr::app
{
class RuntimePanels;
class RigPanel
{
  public:
	RigPanel();
	~RigPanel();
	void Draw(RuntimePanels& runtime);
	void ReplayRecording(RuntimePanels& runtime, const std::string& path, std::uint64_t seed);
	// Called only for an explicit launch action. A rig is never attached to a
	// previously connected application or an ambiguous/stale socket.
	void PrepareLaunch(const LaunchProfile& profile);
	void ObserveLaunch(const LaunchSnapshot& process);

  private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace simxr::app
