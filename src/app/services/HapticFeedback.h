#pragma once

#include "control/HapticObservation.h"

#include <array>
#include <chrono>
#include <cstdint>

namespace simxr::app
{
class HapticFeedback
{
  public:
	using Clock = std::chrono::steady_clock;
	void Observe(control::HapticObservation observation, Clock::time_point requested, Clock::time_point received);
	std::array<std::uint16_t, 2> Motors(Clock::time_point now, bool enabled);
	Clock::time_point Deadline() const noexcept;
	const control::HapticObservation& Recent() const noexcept;
	bool Fresh(Clock::time_point now) const noexcept;
	void Reset();

  private:
	struct Motor
	{
		std::uint64_t sequence = 0;
		std::uint16_t amplitude = 0;
		bool shortPulse = false;
		Clock::time_point end{};
	};
	std::array<Motor, 2> m_motors{};
	control::HapticObservation m_recent;
	Clock::time_point m_received{};
	bool m_observed = false;
};
} // namespace simxr::app
