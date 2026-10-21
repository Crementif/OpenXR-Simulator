#include "presentation/FrameScheduler.h"

#include <algorithm>
#include <limits>
#include <new>
#include <stdexcept>
#include <utility>

namespace simxr
{
FrameScheduler::FrameScheduler(std::shared_ptr<Clock> clock)
	: m_clock(std::move(clock)), m_timeline(m_config.epoch, m_config.period)
{
	if (!m_clock)
		throw std::invalid_argument("Missing frame clock");
}

FrameScheduler::~FrameScheduler()
{
	Stop();
}

void FrameScheduler::Start(const FrameSchedulerConfig& config)
{
	DisplayTimeline timeline(config.epoch, config.period);
	if (config.cadence == 0 || config.cadence > 2 || !config.manualPermitLimit)
		throw std::invalid_argument("Unsupported application cadence or manual permit limit");
	const auto hostEpoch = config.hostEpoch ? *config.hostEpoch : m_clock->Now();
	std::lock_guard lock(m_mutex);
	++m_revision;
	m_config = config;
	m_timeline = timeline;
	m_hostEpoch = hostEpoch;
	m_hostTick = {};
	++m_policyRevision;
	m_nextFrame = 1;
	m_nextAdmission = {};
	m_state = {};
	m_state.running = true;
	m_clock->Wake();
	m_changed.notify_all();
}

void FrameScheduler::Stop()
{
	std::lock_guard lock(m_mutex);
	++m_revision;
	m_state = {};
	m_clock->Wake();
	m_changed.notify_all();
}

FramePipelineState FrameScheduler::Pause()
{
	std::lock_guard lock(m_mutex);
	if (m_state.running && !m_state.paused)
	{
		m_state.paused = true;
		m_state.permits = 0;
		++m_policyRevision;
		m_clock->Wake();
		m_changed.notify_all();
	}
	auto boundary = m_state;
	boundary.nextAdmission = m_nextAdmission;
	return boundary;
}

XrResult FrameScheduler::GrantPermits(std::uint32_t count)
{
	std::lock_guard lock(m_mutex);
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (!m_state.paused)
		return XR_ERROR_CALL_ORDER_INVALID;
	if (!count)
		return XR_ERROR_VALIDATION_FAILURE;
	if (count > m_config.manualPermitLimit - m_state.permits)
		return XR_ERROR_LIMIT_REACHED;
	m_state.permits += count;
	m_clock->Wake();
	m_changed.notify_all();
	return XR_SUCCESS;
}

std::uint32_t FrameScheduler::CancelPermits()
{
	std::lock_guard lock(m_mutex);
	const auto cancelled = m_state.permits;
	m_state.permits = 0;
	m_clock->Wake();
	m_changed.notify_all();
	return cancelled;
}

XrResult FrameScheduler::RebasePaused(DisplayTick boundary)
{
	std::lock_guard lock(m_mutex);
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (!m_state.paused || m_state.permits)
		return XR_ERROR_CALL_ORDER_INVALID;
	boundary = std::max(boundary, m_nextAdmission);
	try
	{
		if (boundary.value > std::numeric_limits<std::uint64_t>::max() - m_config.cadence)
			return XR_ERROR_RUNTIME_FAILURE;
		m_timeline.TimeAt({boundary.value + m_config.cadence});
	}
	catch (const std::overflow_error&)
	{
		return XR_ERROR_RUNTIME_FAILURE;
	}
	m_nextAdmission = boundary;
	++m_policyRevision;
	m_clock->Wake();
	m_changed.notify_all();
	return XR_SUCCESS;
}

XrResult FrameScheduler::Resume(HostTime anchor, DisplayTick boundary)
{
	std::lock_guard lock(m_mutex);
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (!m_state.paused)
		return XR_SUCCESS;
	boundary = std::max(boundary, m_nextAdmission);
	try
	{
		if (boundary.value > std::numeric_limits<std::uint64_t>::max() - m_config.cadence)
			return XR_ERROR_RUNTIME_FAILURE;
		m_timeline.TimeAt({boundary.value + m_config.cadence});
	}
	catch (const std::overflow_error&)
	{
		return XR_ERROR_RUNTIME_FAILURE;
	}
	m_hostEpoch = anchor;
	m_hostTick = boundary;
	m_nextAdmission = boundary;
	m_state.paused = false;
	m_state.permits = 0;
	++m_policyRevision;
	m_clock->Wake();
	m_changed.notify_all();
	return XR_SUCCESS;
}

XrResult FrameScheduler::WaitFrame(FramePrediction& prediction,
								   const std::function<void(const FramePrediction&)>& reserve)
{
	std::unique_lock lock(m_mutex);
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (m_state.waitPending)
		return XR_ERROR_CALL_ORDER_INVALID;
	m_state.waitPending = true;
	const auto revision = m_revision;
	try
	{
		for (;;)
		{
			m_changed.wait(
				lock,
				[&] { return m_revision != revision || (!m_state.waited && (!m_state.paused || m_state.permits)); });
			if (m_revision != revision)
				return XR_ERROR_SESSION_NOT_RUNNING;
			auto admission = m_nextAdmission;
			const auto policy = m_policyRevision;
			if (!m_state.paused && m_config.mode == ExecutionMode::RealTime)
			{
				const auto elapsed =
					std::chrono::duration_cast<Duration>(HostTimeDifference(m_clock->Now(), m_hostEpoch));
				const auto relative = m_timeline.TimeAt(admission).value - m_timeline.TimeAt(m_hostTick).value;
				if (elapsed > relative)
				{
					const auto elapsedTicks = static_cast<std::uint64_t>(elapsed.count() / m_config.period.count());
					const auto extra = static_cast<std::uint64_t>(elapsedTicks != 0);
					if (elapsedTicks > std::numeric_limits<std::uint64_t>::max() - m_hostTick.value - extra)
						throw std::overflow_error("Frame host tick exhausted");
					admission.value = m_hostTick.value + elapsedTicks + extra;
				}
				const auto offset = m_timeline.TimeAt(admission).value - m_timeline.TimeAt(m_hostTick).value;
				if (m_hostEpoch > HostTime::max() - std::chrono::duration_cast<HostTime::duration>(offset))
					throw std::overflow_error("Frame host deadline exhausted");
				const auto deadline = m_hostEpoch + offset;
				while (m_revision == revision && m_policyRevision == policy && m_clock->Now() < deadline)
					m_clock->WaitUntil(m_changed, lock, deadline);
				if (m_revision != revision)
					return XR_ERROR_SESSION_NOT_RUNNING;
				if (m_policyRevision != policy)
					continue;
			}
			if (m_nextFrame == std::numeric_limits<std::uint64_t>::max() ||
				admission.value > std::numeric_limits<std::uint64_t>::max() - m_config.cadence)
				throw std::overflow_error("Frame identities exhausted");
			const DisplayTick display{admission.value + m_config.cadence};
			const FramePrediction candidate{
				{m_nextFrame}, display, m_timeline.TimeAt(display), m_config.period * m_config.cadence};
			if (reserve)
				reserve(candidate);
			prediction = candidate;
			++m_nextFrame;
			m_nextAdmission = display;
			m_state.waited = prediction;
			if (m_state.paused)
				--m_state.permits;
			m_state.waitPending = false;
			return XR_SUCCESS;
		}
	}
	catch (const std::bad_alloc&)
	{
		if (m_revision == revision)
			m_state.waitPending = false;
		return XR_ERROR_OUT_OF_MEMORY;
	}
	catch (...)
	{
		if (m_revision == revision)
			m_state.waitPending = false;
		return XR_ERROR_RUNTIME_FAILURE;
	}
}

XrResult FrameScheduler::BeginFrame()
{
	std::lock_guard lock(m_mutex);
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (!m_state.waited)
		return XR_ERROR_CALL_ORDER_INVALID;
	bool discarded = m_state.begun.has_value();
	m_state.begun = m_state.waited;
	m_state.waited.reset();
	m_clock->Wake();
	m_changed.notify_all();
	return discarded ? XR_FRAME_DISCARDED : XR_SUCCESS;
}

XrResult FrameScheduler::EndFrame(FramePrediction& prediction)
{
	std::lock_guard lock(m_mutex);
	if (!m_state.running)
		return XR_ERROR_SESSION_NOT_RUNNING;
	if (!m_state.begun)
		return XR_ERROR_CALL_ORDER_INVALID;
	prediction = *m_state.begun;
	m_state.begun.reset();
	return XR_SUCCESS;
}

FramePipelineState FrameScheduler::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	auto state = m_state;
	state.nextAdmission = m_nextAdmission;
	return state;
}
} // namespace simxr
