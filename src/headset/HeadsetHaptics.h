#pragma once

#include "control/HapticObservation.h"

#include <array>
#include <chrono>
#include <optional>

namespace simxr::headset
{
struct HeadsetVibration
{
	float amplitude = 0, frequency = 0;
	XrDuration remaining = 0;
};

// One instance per attachment. The first observation establishes the history
// watermark; only subsequent events may reach the physical output.
class HeadsetHaptics
{
public:
	using Clock = std::chrono::steady_clock;
	bool Observe(const control::HapticObservation& observation, Clock::time_point requested, Clock::time_point received,
				 bool enabled = true);
	std::array<std::optional<HeadsetVibration>, 2> Consume(Clock::time_point now, bool enabled);

private:
	struct Hand
	{
		std::uint64_t sequence = 0;
		HeadsetVibration value;
		Clock::time_point deadline;
		bool pending = false, playing = false;
	};

	std::array<Hand, 2> m_hands;
	std::uint64_t m_floor = 0, m_seen = 0;
	std::int64_t m_time = 0;
	Clock::time_point m_received;
	bool m_initialized = false, m_failed = false;
};
} // namespace simxr::headset
