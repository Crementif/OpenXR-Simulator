#pragma once

#include "app/services/RecordingController.h"

namespace simxr::app
{
enum class RecordingCapturePhase
{
	Idle,
	Starting,
	Capturing,
	Stopping,
	Complete,
	Incomplete
};

struct RecordingCaptureSnapshot
{
	RecordingCapturePhase phase = RecordingCapturePhase::Idle;
	bool busy = false;
	std::filesystem::path directory;
	std::uint64_t identity = 0, transactions = 0;
	std::string error;
};

// One worker drains to disk. Target retirement interrupts transport and leaves
// a recoverable prefix; Stop drains and finalizes the current take.
class RecordingCapture
{
public:
	RecordingCapture();
	~RecordingCapture();
	RecordingCapture(const RecordingCapture&) = delete;
	RecordingCapture& operator=(const RecordingCapture&) = delete;

	void SetTarget(RecordingTarget target);
	bool Start(std::filesystem::path directory);
	void Stop();
	RecordingCaptureSnapshot Snapshot() const;

private:
	struct Job
	{
		RecordingTarget target;
		std::filesystem::path directory;
	};

	void Run();
	void Capture(WorkerConnection& connection, const Job& job);

	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::thread m_worker;
	bool m_exiting = false, m_stop = false, m_retired = false;
	RecordingTarget m_target;
	std::optional<Job> m_pending;
	RecordingCaptureSnapshot m_snapshot;
	std::shared_ptr<WorkerConnection> m_current;
};
} // namespace simxr::app
