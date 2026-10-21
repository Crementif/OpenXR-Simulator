#pragma once

#include "simulation/TrackingTypes.h"

#include <cstddef>
#include <deque>
#include <mutex>
#include <optional>

namespace simxr
{
class PoseHistory
{
  public:
	explicit PoseHistory(std::size_t capacity = 128);
	void Push(TrackingSnapshot snapshot, bool replaceCurrent = false);
	std::optional<TrackingSnapshot> Sample(RuntimeTime time) const;
	std::size_t Size() const;
	void Clear();

  private:
	mutable std::mutex m_mutex;
	std::size_t m_capacity;
	std::deque<TrackingSnapshot> m_samples;
};
} // namespace simxr
