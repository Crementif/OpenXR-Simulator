#pragma once

#include "app/services/LaunchAttachment.h"
#include "app/services/AutomationOwner.h"
#include "app/services/WorkerConnection.h"
#include "automation/RigRunner.h"

#include <cstdint>
#include <exception>
#include <filesystem>
#include <future>
#include <optional>
#include <string>

namespace simxr::app
{
struct ScriptOptions
{
	std::uint64_t seed = 0;
	int deadlineSeconds = 30, memoryMiB = 64;
};

struct RigTarget
{
	std::string endpoint;
	std::uint64_t session = 0, generation = 0;
	bool operator==(const RigTarget&) const = default;
};

struct RigReportResult
{
	std::string path;
	std::exception_ptr error;
};

// One execution worker and its transport, with independent owned report output.
class RigController
{
public:
	RigController();
	~RigController();
	void RunScript(RigTarget target, const std::string& path, const ScriptOptions& options);
	std::uint64_t RunSource(RigTarget target, std::string source, std::string name, const ScriptOptions& options,
							const std::shared_ptr<AutomationOwner>& owner, std::uint64_t expectedProcess);
	bool OwnedBy(const std::shared_ptr<AutomationOwner>& owner, std::uint64_t run) const noexcept;
	void CancelDisconnected();
	void ReplayRecording(RigTarget target, const std::string& path, std::uint64_t seed);
	void Pause();
	void Step();
	void Resume();
	void Cancel();
	const std::string& Script() const noexcept;
	bool ReplayingTake() const noexcept;
	bool WaitingForLaunch() const noexcept;
	const std::string& Error() const noexcept;
	// Returns an admission error, or empty once the independent write is started.
	std::string SaveReport(const std::string& path, bool junit);
	bool ReportBusy() const noexcept;
	std::optional<RigReportResult> TakeReportResult();
	bool IsActive() const;
	std::uint64_t RunId() const noexcept;
	automation::RigSnapshot Snapshot() const;
	bool MatchesTarget(const RigTarget& target) const noexcept;
	// A prepared launch owns its script bytes until the matching process attaches.
	void PrepareLaunch(const LaunchProfile& profile);
	void AttachLaunch(const LaunchedSession& session);

private:
	struct Launch
	{
		std::string source, name;
	};

	void Start(RigTarget target, std::string source, std::string name, const ScriptOptions& options, bool replay,
			   std::filesystem::path archive = {}, std::optional<std::uint64_t> expectedProcess = {});
	std::optional<WorkerConnection> m_transport;
	automation::RigRunner m_runner;
	std::optional<Launch> m_launch;
	std::future<RigReportResult> m_report;
	RigTarget m_target;
	std::string m_script, m_error;
	std::uint64_t m_runId = 0;
	std::shared_ptr<AutomationOwner> m_owner;
	bool m_recordingReplay = false;
};
} // namespace simxr::app
