#include "runtime/extensions/EyeGazeInteraction.h"

#include <algorithm>

namespace simxr
{
EyeGazeInteraction::EyeGazeInteraction(const EnabledExtensions& enabled, bool supported) :
	m_enabled(enabled.Contains(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME)), m_supported(supported)
{
}

bool EyeGazeInteraction::Supports() const
{
	return m_supported;
}

void EyeGazeInteraction::ContributeInput(const TrackingSnapshot& snapshot, InputSnapshot& input) const
{
	std::erase_if(input.devices, [](const auto& device) { return device.identity.kind == InputDeviceKind::EyeGaze; });
	if (m_enabled)
		input.devices.push_back({{InputDeviceKind::EyeGaze, 0}, {"/user/eyes_ext"},
			"/interaction_profiles/ext/eye_gaze_interaction", m_supported, snapshot.input.gaze.available, {}});
}

PoseSample EyeGazeInteraction::Pose(const TrackingSnapshot& snapshot) const
{
	if (!m_enabled || !m_supported)
		return {};
	auto result = snapshot.gaze;
	constexpr auto tracked = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	if ((result.flags & tracked) != tracked)
		result.flags &= ~tracked;
	if (!(result.flags & ValidPoseFlags))
		result.eyeGazeSampleTime = RuntimeTime{};
	return result;
}
}
