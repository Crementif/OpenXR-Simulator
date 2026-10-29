#include "simulation/ReferenceSpaceState.h"
#include "Pch.h"
#include "math/Math.h"

namespace simxr
{
bool NormalizeReferenceSpaces(ReferenceSpaceState& state)
{
	const auto local = NormalizedPose(state.local);
	const auto stage = NormalizedPose(state.stage);
	if (!local || !stage)
		return false;
	if (state.stageBounds && (!std::isfinite(state.stageBounds->width) || !std::isfinite(state.stageBounds->height) ||
							  state.stageBounds->width < 0 || state.stageBounds->height < 0))
		return false;
	state.local = *local;
	state.stage = *stage;
	return true;
}

bool RecenterReferenceSpaces(ReferenceSpaceState& state, const PoseSample& head)
{
	if ((head.flags & ValidPoseFlags) != ValidPoseFlags)
		return false;
	const auto pose = NormalizedPose(head.pose);
	if (!pose)
		return false;
	const auto forward = Rotate(pose->orientation, {0, 0, -1});
	const auto rotation = EulerAngles(std::atan2(-forward.x, -forward.z), 0, 0);
	const auto& position = pose->position;
	auto recentered = state;
	recentered.local = {rotation, position};
	recentered.stage = {rotation, {position.x, 0, position.z}};
	if (!NormalizeReferenceSpaces(recentered))
		return false;
	state = recentered;
	return true;
}

bool SameReferenceSpaces(const ReferenceSpaceState& left, const ReferenceSpaceState& right)
{
	const auto components = [](const XrPosef& pose) {
		return std::array{pose.orientation.x, pose.orientation.y, pose.orientation.z, pose.orientation.w,
						  pose.position.x,	  pose.position.y,	  pose.position.z};
	};
	return components(left.local) == components(right.local) && components(left.stage) == components(right.stage) &&
		   left.stageBounds.has_value() == right.stageBounds.has_value() &&
		   (!left.stageBounds || (left.stageBounds->width == right.stageBounds->width &&
								  left.stageBounds->height == right.stageBounds->height));
}
} // namespace simxr
