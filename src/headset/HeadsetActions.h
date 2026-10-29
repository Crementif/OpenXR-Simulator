#pragma once

#include "headset/HeadsetBody.h"
#include "headset/HeadsetSpace.h"
#include "headset/HeadsetTrackers.h"
#include "simulation/InputState.h"

#include <array>
#include <openxr/openxr.h>

namespace simxr::headset
{
class OpenXrRuntime;
enum class HeadsetControllerProfile
{
	Unknown,
	Simple,
	Touch,
	Index,
	Vive,
	Motion
};

struct HeadsetHandSample
{
	XrSpaceLocation grip{XR_TYPE_SPACE_LOCATION};
	XrSpaceLocation aim{XR_TYPE_SPACE_LOCATION};
	XrActionStateBoolean select{XR_TYPE_ACTION_STATE_BOOLEAN};
	XrActionStateFloat trigger{XR_TYPE_ACTION_STATE_FLOAT};
	ControllerControls controls{.connected = false};
	HeadsetControllerProfile profile = HeadsetControllerProfile::Unknown;
};

struct HeadsetEyeSample
{
	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
	XrFovf fov{};
};

struct HeadsetInputSample
{
	XrTime time = 0;
	XrDuration displayPeriod = 0;
	HeadsetSpaceSample space;
	XrReferenceSpaceType referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	XrSpaceLocation head{XR_TYPE_SPACE_LOCATION};
	std::array<HeadsetHandSample, 2> hands;
	std::array<HeadsetEyeSample, 2> eyes;
	std::optional<std::vector<PhysicalTrackerSample>> trackers;
	std::optional<PhysicalBodySample> body;
	bool viewsLocated = false;
	bool focused = false;
	bool controlsReady = false;
};

bool NeutralHeadsetControls(const HeadsetHandSample& hand);

class HeadsetActions
{
public:
	explicit HeadsetActions(OpenXrRuntime& runtime);
	~HeadsetActions();
	HeadsetActions(const HeadsetActions&) = delete;
	HeadsetActions& operator=(const HeadsetActions&) = delete;

	void Open(XrSession session, bool trackers = false);
	HeadsetInputSample Sample(XrTime time, XrSpace baseSpace);

	bool ApplyHaptic(std::uint32_t hand, float amplitude, float frequency, XrDuration remaining);
	bool StopHaptic(std::uint32_t hand);

private:
	XrPath Path(const char* text) const;
	XrAction CreateAction(const char* name, XrActionType type);

	OpenXrRuntime& m_runtime;
	std::optional<HeadsetTrackers> m_trackers;
	XrSession m_session = XR_NULL_HANDLE;
	XrActionSet m_set = XR_NULL_HANDLE;
	XrAction m_grip = XR_NULL_HANDLE;
	XrAction m_aim = XR_NULL_HANDLE;
	XrAction m_select = XR_NULL_HANDLE;
	XrAction m_trigger = XR_NULL_HANDLE;
	XrAction m_triggerClick = XR_NULL_HANDLE;
	XrAction m_squeeze = XR_NULL_HANDLE;
	XrAction m_squeezeClick = XR_NULL_HANDLE;
	XrAction m_stick = XR_NULL_HANDLE;
	XrAction m_gripForce = XR_NULL_HANDLE, m_trackpadForce = XR_NULL_HANDLE, m_trackpad = XR_NULL_HANDLE;
	XrAction m_haptic = XR_NULL_HANDLE;
	std::array<bool, 2> m_hapticActive{};
	bool m_requireNeutral = false;
	std::array<XrAction, 11> m_buttons{};
	XrPath m_viveProfile = XR_NULL_PATH, m_motionProfile = XR_NULL_PATH;
	XrPath m_simpleProfile = XR_NULL_PATH, m_touchProfile = XR_NULL_PATH, m_indexProfile = XR_NULL_PATH;
	std::array<XrPath, 2> m_hands{};
	std::array<XrSpace, 2> m_gripSpaces{};
	std::array<XrSpace, 2> m_aimSpaces{};
	XrSpace m_head = XR_NULL_HANDLE;
};
} // namespace simxr::headset
