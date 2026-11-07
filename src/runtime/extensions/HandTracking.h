#pragma once

#include "runtime/ExtensionRegistry.h"
#include "runtime/HandleRegistry.h"
#include "simulation/InputState.h"
#include "simulation/SpaceSystem.h"

#include <array>
#include <optional>
#include <string_view>

namespace simxr
{
struct HandJointPose
{
	XrPosef pose;
	float radius = 0;
};

using HandJointPoses = std::array<HandJointPose, XR_HAND_JOINT_COUNT_EXT>;

// Joints in the controller's grip space, curled the way the controller senses the fingers.
// side is 0 for the left hand and 1 for the right.
HandJointPoses ControllerHandJoints(std::size_t side, const ControllerControls& controls);

class HandTracking
{
public:
	HandTracking(XrSession owner, const TrackingSystem& tracking, const InputState& input, const SpaceSystem& spaces,
				 const EnabledExtensions& enabled);

	XrResult Create(const XrHandTrackerCreateInfoEXT* info, XrHandTrackerEXT* output);
	XrResult Destroy(XrHandTrackerEXT tracker);
	XrResult Locate(XrHandTrackerEXT tracker, const XrHandJointsLocateInfoEXT* info,
					XrHandJointLocationsEXT* locations) const;

	bool Owns(XrHandTrackerEXT tracker) const;
	void Clear();

private:
	struct Tracker
	{
		std::size_t side = 0;
		bool controllerSource = true;
	};

	bool Enabled(std::string_view extension) const;

	XrSession m_owner;
	const TrackingSystem& m_tracking;
	const InputState& m_input;
	const SpaceSystem& m_spaces;
	const EnabledExtensions m_enabled;
	HandleRegistry<XrHandTrackerEXT, Tracker, XrSession> m_trackers;
};
} // namespace simxr
