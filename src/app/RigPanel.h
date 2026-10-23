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
struct ScriptOptions
{
	std::uint64_t seed = 0;
	int deadlineSeconds = 30, memoryMiB = 64;
};
// Runs one Luau script or take replay at a time against the selected session,
// and draws its status chip. The Library draws the script pages.
class RigPanel
{
  public:
	RigPanel();
	~RigPanel();
	void RunScript(RuntimePanels& runtime, const std::string& path, const ScriptOptions& options);
	void Pause();
	void Step();
	void Resume();
	void Cancel();
	// The path of the current or last run.
	std::string Script() const;
	bool ReplayingTake() const;
	bool WaitingForLaunch() const;
	const std::string& Error() const;
	// Returns the failure, empty on success.
	std::string SaveReport(const std::string& path, bool junit);
	bool IsActive() const;
	std::uint64_t RunId() const;
	automation::RigSnapshot Snapshot() const;
	bool MatchesTarget(const RuntimeView& state) const;
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
