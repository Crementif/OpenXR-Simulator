#pragma once

#include "runtime/EventQueue.h"
#include "simulation/ActionRegistry.h"
#include "simulation/TrackingTypes.h"

#include <cstdint>
#include <mutex>
#include <vector>

namespace simxr
{
struct TrackerPathState
{
	TrackerIdentity identity;
	XrPath persistentPath = XR_NULL_PATH;
	XrPath rolePath = XR_NULL_PATH;
	bool connected = false;
};

class TrackerInteraction
{
  public:
	TrackerInteraction(ActionRegistry& registry, const EnabledExtensions& enabled, EventQueue& events);
	void BeginSession();
	XrResult Publish(const TrackingSnapshot& snapshot, std::vector<XrPath>* changedPaths = nullptr);
	XrResult Enumerate(std::uint32_t capacity, std::uint32_t* count, XrViveTrackerPathsHTCX* paths) const;
	void ContributeInput(const TrackingSnapshot& snapshot, InputSnapshot& input) const;
	PoseSample Pose(const TrackingSnapshot& snapshot, TrackerIdentity identity) const;
	std::vector<TrackerPathState> Paths() const;

  private:
	ActionRegistry& m_registry;
	EventQueue& m_events;
	const bool m_enabled;
	mutable std::mutex m_mutex;
	RuntimeTime m_time;
	std::vector<TrackerPathState> m_paths;
};
} // namespace simxr
