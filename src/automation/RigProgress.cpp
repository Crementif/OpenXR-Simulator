#include "automation/RigProgress.h"
#include "Pch.h"

namespace simxr::automation
{
void RigProgress::Start(const RigOptions& options)
{
	std::lock_guard lock(m_mutex);
	m_cancelled = false;
	m_paused = false;
	m_permits = 0;
	m_result = {};
	m_coverage = {};
	m_background = options.background;
	m_observationTimeout = options.deadline;
	m_result.phase = RigPhase::Running;
	m_result.seed = options.seed;
	m_deadline = options.background ? std::chrono::steady_clock::time_point::max()
									: std::chrono::steady_clock::now() + options.deadline;
}

void RigProgress::Pause()
{
	std::lock_guard lock(m_mutex);
	m_paused = true;
}

void RigProgress::Resume()
{
	{
		std::lock_guard lock(m_mutex);
		m_paused = false;
		m_permits = 0;
	}
	m_changed.notify_all();
}

void RigProgress::Step()
{
	{
		std::lock_guard lock(m_mutex);
		m_paused = true;
		if (m_permits < 64)
			++m_permits;
	}
	m_changed.notify_all();
}

void RigProgress::Cancel()
{
	{
		std::lock_guard lock(m_mutex);
		m_cancelled = true;
	}
	m_changed.notify_all();
}

void RigProgress::Check(bool bindingBoundary)
{
	if (Cancelled())
		throw std::runtime_error("Rig cancelled");
	if (std::chrono::steady_clock::now() >= m_deadline)
		throw std::runtime_error("Rig execution deadline exceeded");
	if (!bindingBoundary)
		return;
	std::unique_lock lock(m_mutex);
	while (m_paused && !m_permits && !Cancelled())
	{
		m_result.phase = RigPhase::Paused;
		m_changed.wait_until(lock, m_deadline);
		if (std::chrono::steady_clock::now() >= m_deadline)
			throw std::runtime_error("Rig execution deadline exceeded while paused");
	}
	if (Cancelled())
		throw std::runtime_error("Rig cancelled");
	if (m_paused && m_permits)
		--m_permits;
	m_result.phase = RigPhase::Running;
}

void RigProgress::WaitUntil(std::chrono::steady_clock::time_point target)
{
	{
		std::unique_lock lock(m_mutex);
		m_changed.wait_until(lock, std::min(target, m_deadline), [&] { return Cancelled(); });
	}
	Check();
}

void RigProgress::Finish(RigPhase phase, std::string error)
{
	{
		std::lock_guard lock(m_mutex);
		m_result.phase = phase;
		m_result.error = std::move(error);
	}
	m_changed.notify_all();
}

bool RigProgress::Cancelled() const
{
	return m_cancelled.load();
}

void RigProgress::SetCompatibility(std::optional<std::vector<std::string>> deviations)
{
	std::lock_guard lock(m_mutex);
	m_result.compatibilityDeviations = std::move(deviations);
}

void RigProgress::TransactionScheduled()
{
	std::lock_guard lock(m_mutex);
	++m_result.transactions;
}

void RigProgress::CaptureRetained()
{
	std::lock_guard lock(m_mutex);
	++m_result.captures;
}

void RigProgress::Checkpoint(RigCheckpoint checkpoint)
{
	const auto passed = checkpoint.passed;
	const auto failure = "Checkpoint failed: " + checkpoint.name + " " + checkpoint.detail;
	{
		std::lock_guard lock(m_mutex);
		if (m_result.checkpoints.size() == 256)
			throw std::length_error("Rig checkpoint capacity exhausted");
		m_result.checkpoints.push_back(std::move(checkpoint));
	}
	if (!passed)
		throw std::runtime_error(failure);
}

void RigProgress::Print(std::string line)
{
	if (line.size() > 4096)
		throw std::length_error("Rig output line exceeds 4096 bytes");
	std::lock_guard lock(m_mutex);
	if (m_result.output.size() == 256)
		throw std::length_error("Rig output capacity exhausted");
	m_result.output.push_back(std::move(line));
}

void RigProgress::ObserveWindow(const std::vector<MonitorFrame>& frames, std::uint64_t analyzed, std::uint64_t skipped,
								std::uint64_t pairs, std::uint64_t gaps)
{
	std::lock_guard lock(m_mutex);
	m_coverage.Add(frames);
	m_coverage.report.lastAnalyzed = analyzed;
	m_coverage.report.lastSkipped = skipped;
	m_coverage.report.lastPairs = pairs;
	m_coverage.report.lastGaps = gaps;
	m_result.monitor = m_coverage.report;
}

void RigProgress::MissedCadences(std::uint64_t count)
{
	std::lock_guard lock(m_mutex);
	m_coverage.report.missedCadences += count;
	m_result.monitor.missedCadences = m_coverage.report.missedCadences;
}

void RigProgress::BeginObservation()
{
	if (m_background && m_deadline == std::chrono::steady_clock::time_point::max())
		m_deadline = std::chrono::steady_clock::now() + m_observationTimeout;
}

void RigProgress::EndObservation()
{
	if (m_background)
		m_deadline = std::chrono::steady_clock::time_point::max();
}

RigPhase RigProgress::Phase() const
{
	std::lock_guard lock(m_mutex);
	return m_result.phase;
}

RigSnapshot RigProgress::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_result;
}
} // namespace simxr::automation
