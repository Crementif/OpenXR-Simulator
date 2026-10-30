#include "headset/HeadsetHaptics.h"
#include "Pch.h"

namespace simxr::headset
{
bool HeadsetHaptics::Observe(const control::HapticObservation& observation, Clock::time_point requested,
							 Clock::time_point received, bool enabled)
{
	// An overflowing observation omits only its oldest events, which were already seen unless the oldest one returned
	// skips past the last sequence observed.
	const bool lost = observation.overflow && m_initialized &&
					  (observation.events.empty() || observation.events.front().sequence > m_seen + 1);
	if (m_failed || lost || requested > received || observation.time < m_time ||
		(m_initialized && received < m_received))
	{
		m_failed = true;
		return false;
	}
	m_time = observation.time;
	m_received = received;
	for (const auto& event : observation.events)
		m_seen = std::max(m_seen, event.sequence);
	if (!m_initialized)
	{
		for (const auto& event : observation.events)
			m_floor = std::max(m_floor, event.sequence);
		m_initialized = true;
		return true;
	}
	if (!enabled)
		for (const auto& event : observation.events)
			m_floor = std::max(m_floor, event.sequence);
	std::array<const control::ObservedHaptic*, 2> latest{};
	for (const auto& event : observation.events)
		if (event.device.kind == InputDeviceKind::Controller && event.device.value < latest.size() &&
			(!latest[event.device.value] || latest[event.device.value]->sequence < event.sequence))
			latest[event.device.value] = &event;
	for (std::size_t index = 0; index < m_hands.size(); ++index)
	{
		auto& hand = m_hands[index];
		const auto* event = latest[index];
		if (!event || event->sequence <= m_floor || event->sequence < hand.sequence ||
			event->status != HapticStatus::Active || event->start > observation.time)
		{
			if (event)
				hand.sequence = std::max(hand.sequence, event->sequence);
			hand.pending = false;
			hand.value = {};
			continue;
		}
		// Repeated polls can shorten a deadline but never restart a pulse.
		const auto age = observation.time - event->start;
		auto remaining = std::max<std::int64_t>(0, 2'000'000'000LL - age);
		if (event->end)
			remaining = std::min(remaining, std::max<std::int64_t>(0, *event->end - observation.time));
		const auto deadline = requested + std::chrono::nanoseconds(remaining);
		if (event->sequence > hand.sequence)
		{
			hand.sequence = event->sequence;
			hand.value = {event->amplitude, event->frequency, remaining};
			hand.deadline = deadline;
			hand.pending = true;
		}
		else
			hand.deadline = std::min(hand.deadline, deadline);
	}
	return true;
}

std::array<std::optional<HeadsetVibration>, 2> HeadsetHaptics::Consume(Clock::time_point now, bool enabled)
{
	std::array<std::optional<HeadsetVibration>, 2> result;
	const bool fresh =
		m_initialized && !m_failed && now >= m_received && now - m_received <= std::chrono::milliseconds(250);
	for (std::size_t index = 0; index < m_hands.size(); ++index)
	{
		auto& hand = m_hands[index];
		if (!enabled || !fresh || now >= hand.deadline || hand.value.amplitude == 0)
		{
			if (hand.playing)
				result[index] = HeadsetVibration{};
			hand.pending = hand.playing = false;
			hand.value = {};
		}
		else if (hand.pending)
		{
			auto value = hand.value;
			value.remaining = std::chrono::duration_cast<std::chrono::nanoseconds>(hand.deadline - now).count();
			result[index] = value;
			hand.pending = false;
			hand.playing = true;
		}
	}
	return result;
}
} // namespace simxr::headset
