#include "simulation/ControllerPoses.h"
#include "Pch.h"
#include "math/Math.h"

namespace simxr
{
PoseSample ControllerAimFromGrip(PoseSample grip, std::string_view profile, bool leftHand)
{
	XrPosef gripInAim = IdentityPose();
	// SteamVR render-model grip/aim components, also used by the upstream runtime.
	if (profile == "/interaction_profiles/oculus/touch_controller" ||
		profile == "/interaction_profiles/meta/touch_pro_controller" ||
		profile == "/interaction_profiles/meta/touch_plus_controller" ||
		profile == "/interaction_profiles/meta/touch_controller_rift_cv1" ||
		profile == "/interaction_profiles/meta/touch_controller_quest_1_rift_s" ||
		profile == "/interaction_profiles/meta/touch_controller_quest_2")
		gripInAim = {{0.5f, 0, 0, 0.8660254f}, {0, -0.0299991f, 0.0950003f}};
	else if (profile == "/interaction_profiles/valve/index_controller")
	{
		const float side = leftHand ? 1.0f : -1.0f;
		gripInAim = {{0.4647712f, 0.0244080f * side, 0.0080248f * side, 0.8850580f},
					 {0.0036100f * side, -0.0707737f, 0.0843448f}};
	}
	else
		return grip;

	if (!(grip.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		grip.flags &= ~(XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
	if (!(grip.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
		grip.flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	grip.pose = Compose(grip.pose, RelativePose(IdentityPose(), gripInAim));
	grip.flags = FinitePoseFlags(grip.pose, grip.flags);
	return grip;
}
} // namespace simxr
