#pragma once

#include "platform/Clock.h"
#include "presentation/FrameScheduler.h"
#include "presentation/SnapshotStore.h"

#include <compare>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace simxr
{
class Session;
struct DebugLeaseId
{
	std::uint64_t value = 0;
	auto operator<=>(const DebugLeaseId&) const = default;
};
struct DebugCommandId
{
	std::uint64_t value = 0;
	auto operator<=>(const DebugCommandId&) const = default;
};
struct DebugOperationId
{
	std::uint64_t value = 0;
	auto operator<=>(const DebugOperationId&) const = default;
};
enum class DebugCommand
{
	Pause,
	Step,
	StrictAdmission,
	DisplayStep,
	Resume,
	Cancel,
	Release,
	StartImageRecording,
	StopImageRecording,
	PinSnapshot,
	UnpinSnapshot,
	ConfigureSnapshots
};
struct DebugRequest
{
	DebugCommandId id;
	DebugCommand command = DebugCommand::Pause;
	std::uint32_t lookahead = 1;
	CaptureTarget target;
	DebugOperationId operation;
	CaptureId capture;
	// Zero keeps direct internal callers unbound. Remote commands bind the run.
	SessionGeneration generation;
	std::uint32_t recordingCapacity = 2;
	std::uint64_t recordingFrames = 0;
	bool advanceDisplay = false;
	std::uint32_t snapshotBudgetMiB = 1024;
	auto operator<=>(const DebugRequest&) const = default;
};
enum class DebugAdmission
{
	Accepted,
	Duplicate,
	Conflict,
	LeaseRequired,
	Capacity,
	Invalid,
	Retired,
	Closed
};
enum class DebugPhase
{
	Queued,
	Pending,
	Complete,
	Failed,
	Cancelled,
	Interrupted
};
enum class DebugProgress
{
	Queued,
	Pausing,
	Paused,
	WaitingSubmission,
	Issued,
	Begun,
	GpuPending,
	SnapshotReady,
	Clear,
	Empty,
	Discarded,
	PipelineStall,
	Displayed,
	Resumed,
	Released,
	Cancelled,
	RetentionFailure,
	Unsupported,
	Busy,
	SessionEnded,
	LeaseExpired,
	RecordingStarted,
	RecordingStopped
};
struct DebugOperation
{
	DebugOperationId id;
	DebugLeaseId lease;
	DebugRequest request;
	DebugPhase phase = DebugPhase::Queued;
	DebugProgress progress = DebugProgress::Queued;
	XrResult result = XR_SUCCESS;
	SnapshotAdmission retention = SnapshotAdmission::Accepted;
	std::optional<FrameKey> frame;
	std::optional<CaptureId> capture;
	std::optional<PresentationTick> display;
	std::vector<CaptureId> captures;
	std::vector<CaptureId> diagnostics;
	std::vector<FrameKey> outstanding;
};
struct DebugSubmission
{
	DebugAdmission result;
	DebugOperationId operation;
};
struct DebugLease
{
	DebugLeaseId id;
	HostTime expires;
};
struct DebugControllerConfig
{
	std::size_t ledgerCapacity = 128;
	std::uint32_t maxLookahead = 4;
	Duration leaseDuration{30000000000};
	Duration pollInterval{1000000};
	bool startWorker = true;
};
struct DebugControllerSnapshot
{
	std::optional<DebugLease> lease;
	bool manual = false;
	std::optional<FrameKey> inspected;
	CaptureTarget inspectedTarget;
	std::uint32_t maxLookahead = 0;
	std::size_t commands = 0;
	bool composedEyes = false;
	bool layerViews = false;
	bool depthViews = false;
	bool filteredComposition = false;
	FramePipelineState pipeline;
	SnapshotStoreState retention;
	ImageRecordingSnapshot imageRecording;
};

// One controller owns one session. Commands are bounded and idempotent for the
// lease's lifetime. Explicit acknowledgement reclaims terminal records and
// retires their command-ID range, so retries cannot execute again.
// The worker only observes existing GPU completion. It never submits or waits
// for GPU work. Holders must release captures explicitly, including after resume.
class DebugController
{
  public:
	DebugController(std::shared_ptr<Session> session, std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>(),
					DebugControllerConfig config = {});
	~DebugController();
	void Close(bool resume = true) noexcept;
	DebugController(const DebugController&) = delete;
	DebugController& operator=(const DebugController&) = delete;
	// Nonzero expected generations serialize with session lifecycle transitions.
	// Lock order is execute -> session lifecycle -> controller state.
	std::optional<DebugLease> Acquire(SessionGeneration expected = {});
	bool Renew(DebugLeaseId lease, SessionGeneration expected = {});
	// Relinquishing a lease requests the same asynchronous recovery as expiry.
	bool Relinquish(DebugLeaseId lease, SessionGeneration expected = {});
	DebugSubmission Submit(DebugLeaseId lease, const DebugRequest& request);
	// A non-null empty destination retains detached native readers for a caller
	// that must release them after dropping its own outer locks.
	bool Acknowledge(DebugLeaseId lease, DebugCommandId through, std::vector<DebugOperation>* retired = nullptr,
					 SessionGeneration expected = {});
	std::optional<DebugOperation> Read(DebugOperationId operation) const;
	DebugControllerSnapshot Snapshot() const;
	// Also exposed for deterministic tests; serialized with the polling worker.
	void Poll() noexcept;

  private:
	void Run() noexcept;
	void Process(DebugOperation& operation);
	void Settle(DebugOperation& operation);
	void Fail(DebugOperation& operation, DebugProgress progress, XrResult result);
	bool ResumeSession();
	void Interrupt(DebugLeaseId lease, DebugProgress reason, std::optional<DebugOperationId> except = {});
	void Publish(DebugOperation operation);
	bool OtherAdmission(DebugOperationId current) const;
	std::vector<SnapshotRecord> OrderedCaptures(CaptureTarget target) const;
	std::vector<SnapshotRecord> Candidates(DebugOperation& operation) const;
	void Own(DebugLeaseId lease, CaptureId capture);
	void RetireOwned(std::optional<DebugLeaseId> lease = {});
	void Inspect(DebugOperation& operation, const SnapshotRecord& record);
	std::shared_ptr<Session> m_session;
	std::shared_ptr<Clock> m_clock;
	const DebugControllerConfig m_config;
	// Execution may call Session, scheduler and display. State is only held for
	// short ledger/lease copies, never across those calls or a worker join.
	std::mutex m_lifecycle;
	mutable std::mutex m_state;
	std::mutex m_execute;
	std::condition_variable m_changed;
	std::optional<DebugLease> m_lease;
	std::deque<DebugOperation> m_operations;
	std::uint64_t m_nextLease = 1;
	std::uint64_t m_nextOperation = 1;
	DebugCommandId m_retiredFloor;
	bool m_stopping = false;
	bool m_recovering = false;
	bool m_publishedManual = false;
	std::optional<FrameKey> m_publishedCursor;
	CaptureTarget m_publishedTarget;
	std::thread m_worker;
	struct OwnedCapture
	{
		DebugLeaseId lease;
		CaptureId capture;
	};
	std::vector<OwnedCapture> m_owned;
	// Protected by execute, not state.
	bool m_manual = false;
	std::optional<DebugLeaseId> m_recordingLease;
	bool m_boundaryCaptured = false;
	SessionGeneration m_generation;
	std::optional<FrameKey> m_cursor;
	CaptureTarget m_cursorTarget;
	struct TargetCursor
	{
		CaptureTarget target;
		FrameKey frame;
	};
	std::vector<TargetCursor> m_cursors;
};
} // namespace simxr
