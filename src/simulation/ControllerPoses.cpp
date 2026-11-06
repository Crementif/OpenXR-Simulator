#include "simulation/ControllerPoses.h"
#include "Pch.h"
#include "math/Math.h"

namespace simxr
{
static std::optional<XrPosef> GripInAim(std::string_view profile, bool leftHand)
{
	// SteamVR render-model grip/aim components, also used by the upstream runtime.
	if (profile == "/interaction_profiles/oculus/touch_controller" ||
		profile == "/interaction_profiles/meta/touch_pro_controller" ||
		profile == "/interaction_profiles/meta/touch_plus_controller" ||
		profile == "/interaction_profiles/meta/touch_controller_rift_cv1" ||
		profile == "/interaction_profiles/meta/touch_controller_quest_1_rift_s" ||
		profile == "/interaction_profiles/meta/touch_controller_quest_2")
		return XrPosef{{0.5f, 0, 0, 0.8660254f}, {0, -0.0299991f, 0.0950003f}};
	if (profile == "/interaction_profiles/valve/index_controller")
	{
		const float side = leftHand ? 1.0f : -1.0f;
		return XrPosef{{0.4647712f, 0.0244080f * side, 0.0080248f * side, 0.8850580f},
					   {0.0036100f * side, -0.0707737f, 0.0843448f}};
	}
	return std::nullopt;
}

PoseSample ControllerAimFromGrip(PoseSample grip, std::string_view profile, bool leftHand)
{
	const auto gripInAim = GripInAim(profile, leftHand);
	if (!gripInAim)
		return grip;
	if (!(grip.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		grip.flags &= ~(XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
	if (!(grip.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
		grip.flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	grip.pose = Compose(grip.pose, RelativePose(IdentityPose(), *gripInAim));
	grip.flags = FinitePoseFlags(grip.pose, grip.flags);
	return grip;
}

XrPosef ControllerGripFromAim(XrPosef aim, std::string_view profile, bool leftHand)
{
	const auto gripInAim = GripInAim(profile, leftHand);
	return gripInAim ? Compose(aim, *gripInAim) : aim;
}

XrVector3f ViewRayPoint(XrPosef eye, XrFovf fov, XrVector2f uv, float distance)
{
	const float left = std::tan(fov.angleLeft), right = std::tan(fov.angleRight);
	const float up = std::tan(fov.angleUp), down = std::tan(fov.angleDown);
	XrVector3f direction{left + uv.x * (right - left), up - uv.y * (up - down), -1};
	const float scale =
		distance / std::sqrt(direction.x * direction.x + direction.y * direction.y + direction.z * direction.z);
	return Compose(eye, {{0, 0, 0, 1}, {direction.x * scale, direction.y * scale, direction.z * scale}}).position;
}

XrQuaternionf LookAt(XrVector3f origin, XrVector3f target)
{
	const XrVector3f direction{target.x - origin.x, target.y - origin.y, target.z - origin.z};
	const float flat = std::hypot(direction.x, direction.z);
	if (flat + std::abs(direction.y) < 1e-6f)
		return {0, 0, 0, 1};
	return EulerAngles(std::atan2(-direction.x, -direction.z), std::atan2(direction.y, flat), 0);
}
} // namespace simxr
