#pragma once

#include "simulation/BodyModel.h"
#include "simulation/PoseHistory.h"

#include <cstddef>
#include <mutex>
#include <optional>

namespace simxr
{
class TrackingSystem
{
public:
	explicit TrackingSystem(std::size_t historyCapacity = 128);

	static TrackingSnapshot Evaluate(RuntimeTime time, const TrackingState& state, const BodyModel& body);

	TrackingSnapshot Publish(RuntimeTime time, const TrackingState& state, bool replaceCurrent = false);
	std::optional<TrackingSnapshot> Sample(RuntimeTime time) const;

	void SetBodyProportions(const BodyProportions& proportions);
	void Reset();

private:
	mutable std::mutex m_mutex;
	BodyModel m_body;
	PoseHistory m_history;
};
} // namespace simxr
