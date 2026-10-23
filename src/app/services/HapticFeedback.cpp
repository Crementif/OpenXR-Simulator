#include "app/services/HapticFeedback.h"
#include "Pch.h"

#include <cmath>

namespace simxr::app
{
void HapticFeedback::Observe(control::HapticObservation observation, Clock::time_point requested,
							 Clock::time_point received)
{
	std::array<const control::ObservedHaptic*, 2> latest{};
	for (const auto& event : observation.events)
		if (event.device.kind == InputDeviceKind::Controller && event.device.value < latest.size())
			latest[event.device.value] = &event;
	for (std::size_t hand = 0; hand < latest.size(); ++hand)
	{
		auto& motor = m_motors[hand];
		const auto* event = latest[hand];
		if (!event)
		{
			motor.amplitude = 0;
			continue;
		}
		if (event->sequence < motor.sequence)
			continue;
		const bool newEvent = event->sequence > motor.sequence;
		if (event->status == HapticStatus::Expired && event->end && *event->end <= observation.time &&
			event->start <= *event->end)
		{
			if (newEvent)
			{
				motor.sequence = event->sequence;
				motor.shortPulse = Fresh(received) && *event->end - event->start <= 100'000'000 &&
								   observation.time - *event->end <= 100'000'000 &&
								   received - requested <= std::chrono::milliseconds(100);
				motor.amplitude =
					motor.shortPulse ? static_cast<std::uint16_t>(std::lround(event->amplitude * 65535.0f)) : 0;
				motor.end = received + std::chrono::nanoseconds(
										   std::min<std::int64_t>(*event->end - event->start, 2'000'000'000LL));
			}
			else if (!motor.shortPulse)
				motor.amplitude = 0;
			continue;
		}
		if (event->status != HapticStatus::Active || event->start > observation.time)
		{
			motor.sequence = event->sequence;
			motor.amplitude = 0;
			continue;
		}
		const auto age = observation.time - event->start;
		auto remaining = std::max<std::int64_t>(0, 2'000'000'000LL - age);
		if (event->end)
			remaining = std::min(remaining, std::max<std::int64_t>(0, *event->end - observation.time));
		// Use request time conservatively; transport delay never extends an event.
		const auto end = requested + std::chrono::nanoseconds(remaining);
		if (event->sequence > motor.sequence)
		{
			motor.sequence = event->sequence;
			motor.shortPulse = false;
			motor.amplitude = static_cast<std::uint16_t>(std::lround(event->amplitude * 65535.0f));
			motor.end = end;
		}
		else
			motor.end = std::min(motor.end, end);
	}
	m_received = received;
	m_observed = true;
	m_recent = std::move(observation);
}
std::array<std::uint16_t, 2> HapticFeedback::Motors(Clock::time_point now, bool enabled)
{
	std::array<std::uint16_t, 2> result{};
	for (std::size_t hand = 0; hand < result.size(); ++hand)
	{
		auto& motor = m_motors[hand];
		if (!enabled || !Fresh(now) || now >= motor.end)
			motor.amplitude = 0;
		result[hand] = motor.amplitude;
	}
	return result;
}
HapticFeedback::Clock::time_point HapticFeedback::Deadline() const noexcept
{
	auto deadline = Clock::time_point::max();
	for (const auto& motor : m_motors)
		if (motor.amplitude)
			deadline = std::min(deadline, motor.end);
	return deadline;
}
const control::HapticObservation& HapticFeedback::Recent() const noexcept
{
	return m_recent;
}
bool HapticFeedback::Fresh(Clock::time_point now) const noexcept
{
	return m_observed && now - m_received <= std::chrono::milliseconds(250);
}
void HapticFeedback::Reset()
{
	*this = {};
}
} // namespace simxr::app
