#include "graphics/RotationalReprojection.h"
#include "Pch.h"
#include "Math.h"

namespace simxr
{
XrResult PrepareRotationalComposition(const FrameLayers& layers, const CompositionTarget& submittedTarget,
									  const PoseSample& submittedHead, const PoseSample& displayHead,
									  const CompositionExtentResolver& resolve, CompositionPlan& output)
{
	if ((submittedHead.flags & ValidPoseFlags) != ValidPoseFlags ||
		!(displayHead.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		return XR_ERROR_POSE_INVALID;
	const auto oldHead = NormalizedPose(submittedHead.pose);
	const auto newHead = NormalizedPose({displayHead.pose.orientation, submittedHead.pose.position});
	if (!oldHead || !newHead)
		return XR_ERROR_POSE_INVALID;
	auto target = submittedTarget;
	for (auto& eye : target.eyes)
	{
		const auto pose = NormalizedPose(eye.worldPose.pose);
		if (!pose || !(eye.worldPose.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
			return XR_ERROR_POSE_INVALID;
		// Preserve the eye offset without constructing an inverse world-space translation.
		eye.worldPose.pose = Compose(*newHead, RelativePose(*pose, *oldHead));
	}
	auto descriptions = layers.layers;
	for (auto& layer : descriptions)
	{
		const auto result = std::visit(
			[&](auto& description) -> XrResult
			{
				auto& space = description.space;
				if (space.referenceType == XR_REFERENCE_SPACE_TYPE_LOCAL ||
					space.referenceType == XR_REFERENCE_SPACE_TYPE_STAGE)
					return XR_SUCCESS;
				if (space.referenceType != XR_REFERENCE_SPACE_TYPE_VIEW)
					return XR_ERROR_FEATURE_UNSUPPORTED;
				const auto offset = NormalizedPose(space.poseOffset);
				if (!offset)
					return XR_ERROR_POSE_INVALID;
				space.worldPose.pose = Compose(*newHead, *offset);
				return XR_SUCCESS;
			},
			layer);
		if (XR_FAILED(result))
			return result;
		if (auto* projection = std::get_if<ProjectionLayerDescription>(&layer))
		{
			if (projection->space.referenceType == XR_REFERENCE_SPACE_TYPE_VIEW)
				for (auto& view : projection->views)
					view.worldPose.pose = Compose(projection->space.worldPose.pose, view.poseInSpace);
		}
		else
		{
			auto& quad = std::get<QuadLayerDescription>(layer);
			if (quad.space.referenceType == XR_REFERENCE_SPACE_TYPE_VIEW)
				quad.worldPose.pose = Compose(quad.space.worldPose.pose, quad.poseInSpace);
		}
	}
	return PrepareComposition(FrameLayers{layers.displayTime, layers.blendMode, std::move(descriptions)}, target,
							  resolve, output);
}
} // namespace simxr
