#include "presentation/VirtualDisplay.h"

#include <limits>
#include <stdexcept>
#include <utility>

namespace simxr
{
VirtualDisplay::VirtualDisplay(std::shared_ptr<Clock> clock, std::size_t capacity, FrameHistory* history)
	: m_clock(std::move(clock)), m_controller(capacity, history)
{
	if (!m_clock)
		throw std::invalid_argument("Missing virtual display clock");
	m_waiter = m_clock->IndependentWaiter();
	if (!m_waiter)
		m_waiter = m_clock;
}

VirtualDisplay::~VirtualDisplay()
{
	Stop();
}

void VirtualDisplay::Start(SessionGeneration generation, const DisplayTimeline& timeline, ExecutionMode mode,
						   std::optional<HostTime> hostEpoch)
{
	const auto anchor = hostEpoch ? *hostEpoch : m_clock->Now();
	std::lock_guard control(m_control);
	StopLocked();
	m_controller.Start(generation, timeline);
	{
		std::lock_guard lock(m_mutex);
		m_timeline = timeline;
		m_mode = mode;
		m_hostEpoch = anchor;
		m_hostTick = {};
		m_active = true;
		m_paused = false;
		m_faulted.store(false);
	}
	try
	{
		if (mode == ExecutionMode::RealTime)
			m_worker = std::thread([this] { Run(); });
	}
	catch (...)
	{
		StopLocked();
		throw;
	}
}

void VirtualDisplay::StopLocked() noexcept
{
	{
		std::lock_guard lock(m_mutex);
		m_active = false;
		m_changed.notify_all();
		try
		{
			m_waiter->Wake();
		}
		catch (...)
		{
			m_faulted.store(true);
		}
	}
	m_controller.Stop();
	if (m_worker.joinable())
		m_worker.join();
}

void VirtualDisplay::Stop() noexcept
{
	std::lock_guard control(m_control);
	StopLocked();
}

bool VirtualDisplay::Pause()
{
	std::lock_guard control(m_control);
	{
		std::lock_guard lock(m_mutex);
		if (!m_active)
			return false;
		m_paused = true;
		m_changed.notify_all();
		try
		{
			m_waiter->Wake();
		}
		catch (...)
		{
			m_active = false;
			m_faulted.store(true);
		}
	}
	if (m_worker.joinable())
		m_worker.join();
	if (m_faulted.load())
		m_controller.Stop();
	return !m_faulted.load();
}

bool VirtualDisplay::Resume(HostTime hostAnchor, DisplayTick boundary)
{
	std::lock_guard control(m_control);
	std::optional<DisplayTimeline> timeline;
	{
		std::lock_guard lock(m_mutex);
		if (!m_active || m_faulted.load())
			return false;
		if (!m_paused)
			return true;
		timeline = m_timeline;
	}
	if (boundary < NextTick())
		return false;
	timeline->TimeAt(boundary);
	{
		std::lock_guard lock(m_mutex);
		m_hostEpoch = hostAnchor;
		m_hostTick = boundary;
		m_paused = false;
	}
	try
	{
		if (m_mode == ExecutionMode::RealTime)
			m_worker = std::thread([this] { Run(); });
	}
	catch (...)
	{
		std::lock_guard lock(m_mutex);
		m_paused = true;
		throw;
	}
	return true;
}

bool VirtualDisplay::Paused() const
{
	std::lock_guard lock(m_mutex);
	return m_active && m_paused;
}

DisplayTick VirtualDisplay::NextTick() const
{
	const auto latest = m_controller.Snapshot().latest;
	if (latest && latest->tick.value == std::numeric_limits<std::uint64_t>::max())
		throw std::overflow_error("Virtual display tick exhausted");
	return {latest ? latest->tick.value + 1 : 0};
}

std::optional<PresentationTick> VirtualDisplay::Step()
{
	std::lock_guard control(m_control);
	std::optional<DisplayTimeline> timeline;
	{
		std::lock_guard lock(m_mutex);
		if (!m_active || !m_paused)
			return {};
		timeline = m_timeline;
	}
	return m_controller.Advance(timeline->TimeAt(NextTick()));
}

PresentationAdmission VirtualDisplay::Submit(PresentationSubmission submission, const std::function<void()>& commit)
{
	return m_controller.Submit(std::move(submission), commit);
}

PresentationAdmission
VirtualDisplay::SubmitWithMetadata(PresentationSubmission submission,
								   const std::function<std::shared_ptr<const FrameRecord>()>& commit)
{
	return m_controller.SubmitWithMetadata(std::move(submission), commit);
}

PresentationAdmission
VirtualDisplay::SubmitLiveClearWithMetadata(PresentationSubmission submission,
											const std::function<std::shared_ptr<const FrameRecord>()>& commit)
{
	return m_controller.SubmitLiveClearWithMetadata(std::move(submission), commit);
}

std::vector<PresentationSubmission> VirtualDisplay::Retain() const
{
	return m_controller.Retain();
}

std::optional<PresentationTick> VirtualDisplay::Advance(RuntimeTime time)
{
	std::lock_guard control(m_control);
	{
		std::lock_guard lock(m_mutex);
		if (!m_active || (m_mode == ExecutionMode::RealTime && !m_paused))
			return {};
	}
	return m_controller.Advance(time);
}

void VirtualDisplay::AdvanceFromFrame(SessionGeneration generation, RuntimeTime time) noexcept
{
	std::lock_guard control(m_control);
	{
		std::lock_guard lock(m_mutex);
		if (!m_active || m_paused || m_mode == ExecutionMode::RealTime)
			return;
	}
	try
	{
		m_controller.Advance(time, generation);
	}
	catch (...)
	{
		m_faulted.store(true);
		{
			std::lock_guard lock(m_mutex);
			m_active = false;
		}
		m_controller.Stop();
	}
}

void VirtualDisplay::Run() noexcept
{
	try
	{
		std::unique_lock lock(m_mutex);
		while (m_active && !m_paused)
		{
			const auto timeline = *m_timeline;
			const auto anchor = m_hostEpoch;
			const auto anchorTick = m_hostTick;
			const auto hostNow = m_clock->Now();
			if (hostNow < anchor)
			{
				m_waiter->WaitUntil(m_changed, lock, anchor);
				continue;
			}
			const auto elapsed = std::chrono::duration_cast<Duration>(hostNow - anchor);
			const auto anchorTime = timeline.TimeAt(anchorTick);
			if (elapsed.count() > std::numeric_limits<std::int64_t>::max() - anchorTime.value.count())
				throw std::overflow_error("Virtual display time exhausted");
			const RuntimeTime now{anchorTime.value + elapsed};
			lock.unlock();
			m_controller.Advance(now);
			lock.lock();
			if (!m_active || m_paused)
				break;
			const auto elapsedTicks = static_cast<std::uint64_t>(elapsed.count() / timeline.Period().count());
			if (anchorTick.value >= std::numeric_limits<std::uint64_t>::max() - elapsedTicks)
				throw std::overflow_error("Virtual display tick exhausted");
			const DisplayTick next{anchorTick.value + elapsedTicks + 1};
			const auto offset = timeline.TimeAt(next).value - anchorTime.value;
			if (anchor > HostTime::max() - std::chrono::duration_cast<HostTime::duration>(offset))
				throw std::overflow_error("Virtual display host deadline exhausted");
			const auto deadline = anchor + offset;
			while (m_active && !m_paused && m_clock->Now() < deadline)
				m_waiter->WaitUntil(m_changed, lock, deadline);
		}
	}
	catch (...)
	{
		m_faulted.store(true);
		{
			std::lock_guard lock(m_mutex);
			m_active = false;
		}
		m_controller.Stop();
	}
}

PresentationSnapshot VirtualDisplay::Snapshot() const
{
	return m_controller.Snapshot();
}

bool VirtualDisplay::Faulted() const noexcept
{
	return m_faulted.load();
}

std::size_t VirtualDisplay::PollRetired()
{
	return m_controller.PollRetired();
}
} // namespace simxr
