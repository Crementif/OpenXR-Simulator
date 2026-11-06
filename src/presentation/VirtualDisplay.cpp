#include "presentation/VirtualDisplay.h"
#include "Pch.h"

#include <new>

namespace simxr
{
VirtualDisplay::VirtualDisplay(std::shared_ptr<Clock> clock, std::size_t capacity, FrameHistory* history,
							   PresentationController::CompletionObserver completed)
	: m_clock(std::move(clock)), m_controller(capacity, history, std::move(completed))
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

void VirtualDisplay::ConfigurePolicy(PresentationPolicy policy, HeadSampler sampleHead)
{
	if (policy != PresentationPolicy::Repeat && policy != PresentationPolicy::Rotational)
		throw std::invalid_argument("Unknown presentation policy");
	if (policy == PresentationPolicy::Rotational && !sampleHead)
		throw std::invalid_argument("Rotational display requires a head sampler");
	std::lock_guard control(m_control);
	{
		std::lock_guard lock(m_mutex);
		if (m_active)
			throw std::logic_error("Configure presentation policy while stopped");
	}
	// A faulted worker can be inactive but still returning through its catch path.
	StopLocked();
	m_sampleHead = std::move(sampleHead);
	std::lock_guard lock(m_mutex);
	m_policy = policy;
}

PresentationPolicy VirtualDisplay::Policy() const
{
	std::lock_guard lock(m_mutex);
	return m_policy;
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
		m_generation = generation;
		m_external.reset();
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
			m_worker = std::thread(&VirtualDisplay::Run, this);
	}
	catch (...)
	{
		StopLocked();
		throw;
	}
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
		m_external.reset();
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
			m_worker = std::thread(&VirtualDisplay::Run, this);
	}
	catch (...)
	{
		std::lock_guard lock(m_mutex);
		m_paused = true;
		throw;
	}
	return true;
}

bool VirtualDisplay::Retime(SessionGeneration generation, const DisplayTimeline& timeline)
{
	std::lock_guard control(m_control);
	{
		std::lock_guard lock(m_mutex);
		if (!m_active || generation != m_generation)
			return false;
		m_timeline = timeline;
		m_changed.notify_all();
	}
	m_controller.Retime(generation, timeline);
	m_waiter->Wake();
	return true;
}

bool VirtualDisplay::Paused() const
{
	std::lock_guard lock(m_mutex);
	return m_active && m_paused;
}

bool VirtualDisplay::PublishExternalPrediction(SessionGeneration generation, std::uint64_t attachment,
											   std::uint64_t sequence, RuntimeTime target, Duration period)
{
	std::lock_guard lock(m_mutex);
	if (!m_active || m_paused || m_mode != ExecutionMode::RealTime || generation != m_generation || !attachment ||
		!sequence || period < std::chrono::milliseconds(1) || period > std::chrono::milliseconds(100))
		return false;
	const auto now = m_clock->Now();
	const auto elapsed = std::chrono::duration_cast<Duration>(HostTimeDifference(now, m_hostEpoch));
	const auto anchor = m_timeline->TimeAt(m_hostTick).value;
	if (elapsed.count() < 0 || elapsed > Duration::max() - anchor)
		return false;
	const RuntimeTime runtimeNow{anchor + elapsed};
	if (target <= runtimeNow || target.value - runtimeNow.value > period * 4 || now > HostTime::max() - period * 4)
		return false;
	if (m_external && now >= m_external->expires)
		m_external.reset();
	if (m_external &&
		(m_external->attachment != attachment || sequence <= m_external->sequence || target <= m_external->target))
		return false;
	m_waiter->Wake();
	const auto consumed = m_external ? m_external->consumed : 0;
	m_external = ExternalPrediction{attachment, sequence,		  consumed,
									target,		now + period * 4, now + (target.value - runtimeNow.value)};
	m_changed.notify_all();
	return true;
}

void VirtualDisplay::ReleaseExternalPrediction(std::uint64_t attachment)
{
	std::lock_guard lock(m_mutex);
	if (!m_external || m_external->attachment != attachment)
		return;
	m_external.reset();
	m_waiter->Wake();
	m_changed.notify_all();
}

DisplayTick VirtualDisplay::NextTick() const
{
	std::optional<DisplayTimeline> timeline;
	{
		std::lock_guard lock(m_mutex);
		timeline = m_timeline;
	}
	const auto latest = m_controller.Snapshot().latest;
	if (!latest || !timeline)
		return {};
	auto next = timeline->TickAtOrAfter(latest->time);
	if (timeline->TimeAt(next) <= latest->time)
	{
		if (next.value == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Virtual display tick exhausted");
		++next.value;
	}
	return next;
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
	return AdvanceTick(timeline->TimeAt(NextTick()));
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
	return AdvanceTick(time);
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
		AdvanceTick(time, generation);
	}
	catch (...)
	{
		m_faulted.store(true);
		{
			std::lock_guard lock(m_mutex);
			m_active = false;
		}
		m_controller.Stop();
		m_reprojection.Reset();
	}
}

PresentationSnapshot VirtualDisplay::Snapshot() const
{
	return m_controller.Snapshot();
}

void VirtualDisplay::WaitForImage(std::optional<DisplayTick> after,
								  std::chrono::steady_clock::time_point deadline) const
{
	m_controller.WaitForImage(after, deadline);
}

bool VirtualDisplay::Faulted() const noexcept
{
	return m_faulted.load();
}

std::size_t VirtualDisplay::PollRetired()
{
	return m_controller.PollRetired();
}

std::optional<PresentationTick> VirtualDisplay::AdvanceTick(RuntimeTime time,
															std::optional<SessionGeneration> generation, bool exact,
															const std::function<bool()>& current)
{
	if (m_policy == PresentationPolicy::Repeat)
		return exact ? m_controller.AdvanceExternal(time, *generation, {}, current)
					 : m_controller.Advance(time, generation);
	const auto reproject = [this](const PresentationTick& baseline) {
		PoseSample head;
		try
		{
			if (baseline.output && baseline.reprojection.source)
				head = m_sampleHead(baseline.time);
		}
		catch (const std::bad_alloc&)
		{
			auto fallback = m_reprojection.Process(baseline, {});
			fallback.result = XR_ERROR_OUT_OF_MEMORY;
			return fallback;
		}
		return m_reprojection.Process(baseline, head);
	};
	return exact ? m_controller.AdvanceExternal(time, *generation, reproject, current)
				 : m_controller.Advance(time, generation, reproject);
}

void VirtualDisplay::StopLocked() noexcept
{
	{
		std::lock_guard lock(m_mutex);
		m_active = false;
		m_external.reset();
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
	m_reprojection.Reset();
}

void VirtualDisplay::Run() noexcept
{
	try
	{
		std::unique_lock lock(m_mutex);
		while (m_active && !m_paused)
		{
			if (m_external)
			{
				if (m_clock->Now() >= m_external->expires)
					m_external.reset();
				else if (m_external->sequence != m_external->consumed)
				{
					const auto pulse = *m_external;
					const auto target = pulse.target;
					const auto generation = m_generation;
					m_external->consumed = m_external->sequence;
					if (m_clock->Now() >= m_external->targetHost)
						continue;
					lock.unlock();
					AdvanceTick(target, generation, true, [&] {
						std::lock_guard state(m_mutex);
						return m_active && !m_paused && m_generation == generation && m_external &&
							   m_external->attachment == pulse.attachment && m_external->sequence == pulse.sequence &&
							   m_clock->Now() < pulse.targetHost;
					});
					lock.lock();
					continue;
				}
				else
				{
					m_waiter->WaitUntil(m_changed, lock, m_external->expires);
					continue;
				}
			}
			const auto timeline = *m_timeline;
			const auto anchor = m_hostEpoch;
			const auto anchorTick = m_hostTick;
			const auto hostNow = m_clock->Now();
			if (hostNow < anchor)
			{
				m_waiter->WaitUntil(m_changed, lock, anchor);
				continue;
			}
			const auto elapsed = std::chrono::duration_cast<Duration>(HostTimeDifference(hostNow, anchor));
			const auto anchorTime = timeline.TimeAt(anchorTick);
			if (elapsed.count() > std::numeric_limits<std::int64_t>::max() - anchorTime.value.count())
				throw std::overflow_error("Virtual display time exhausted");
			const RuntimeTime now{anchorTime.value + elapsed};
			lock.unlock();
			AdvanceTick(now);
			lock.lock();
			if (!m_active || m_paused)
				break;
			const auto reached = timeline.TickAtOrBefore(now);
			if (reached.value == std::numeric_limits<std::uint64_t>::max())
				throw std::overflow_error("Virtual display tick exhausted");
			const DisplayTick next{std::max(reached.value, anchorTick.value) + 1};
			const auto offset = timeline.TimeAt(next).value - anchorTime.value;
			if (anchor > HostTime::max() - std::chrono::duration_cast<HostTime::duration>(offset))
				throw std::overflow_error("Virtual display host deadline exhausted");
			const auto deadline = anchor + offset;
			while (m_active && !m_paused && !m_external && m_clock->Now() < deadline)
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
		m_reprojection.Reset();
	}
}

} // namespace simxr
