#pragma once

#include "app/CompanionSettings.h"
#include "app/LaunchTypes.h"
#include "automation/RigRunner.h"

#include <cstdint>
#include <memory>
#include <string>

namespace simxr::app
{
class RuntimePanels;
struct RuntimeView;
struct LaunchedSession;
class RigPanel
{
  public:
	RigPanel();
	~RigPanel();
	void Draw(RuntimePanels& runtime, bool visible = true);
	bool IsActive() const;
	std::uint64_t RunId() const;
	automation::RigSnapshot Snapshot() const;
	bool MatchesTarget(const RuntimeView& state) const;
	bool HasStatus() const;
	void DrawTransport();
	void ReplayRecording(RuntimePanels& runtime, const std::string& path, std::uint64_t seed);
	// Called only for an explicit launch action. A rig is never attached to a
	// previously connected application or an ambiguous/stale socket.
	void PrepareLaunch(const LaunchProfile& profile);
	void AttachLaunch(const LaunchedSession& session);

  private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace simxr::app
