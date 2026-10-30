#pragma once

#include "automation/RigTypes.h"

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <mutex>
#include <string>

namespace simxr::automation
{
class RigProgress
{
public:
	void Start(const RigOptions& options);
	void Pause();
	void Resume();
	void Step();
	void Cancel();

	void Check(bool bindingBoundary = false);
	void WaitUntil(std::chrono::steady_clock::time_point target);
	void Finish(RigPhase phase, std::string error);
	bool Cancelled() const;

	void SetCompatibility(std::optional<std::vector<std::string>> deviations);
	void TransactionScheduled();
	void CaptureRetained();
	void Checkpoint(RigCheckpoint checkpoint);
	void Print(std::string line);

	RigPhase Phase() const;
	RigSnapshot Snapshot() const;

private:
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::atomic<bool> m_cancelled{false};
	bool m_paused = false;
	std::size_t m_permits = 0;
	std::chrono::steady_clock::time_point m_deadline;
	RigSnapshot m_result;
};
} // namespace simxr::automation
