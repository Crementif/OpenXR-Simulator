#include "simulation/BodyObservation.h"
#include "Pch.h"
#include "math/Math.h"
#include "simulation/BodyModel.h"

namespace simxr
{
BodyObservation NormalizeBodyObservation(BodyObservation observation)
{
	const auto map = BodyModel::JointMap(observation.jointSet);
	const bool fb =
		observation.jointSet == BodyJointSet::UpperBodyFB || observation.jointSet == BodyJointSet::FullBodyMETA;
	if (observation.requestedTime.value.count() <= 0 ||
		(observation.returnedTime && (observation.returnedTime->value.count() < 0 ||
									  (observation.active == true && observation.returnedTime->value.count() == 0))))
		throw std::invalid_argument("Invalid body observation timestamp");
	if (observation.located)
	{
		if (fb ? (!observation.active || !observation.confidence || !observation.returnedTime ||
				  !observation.skeletonRevision || !observation.skeleton ||
				  observation.allJointPosesTracked.has_value())
			   : (!observation.allJointPosesTracked || observation.active || observation.confidence ||
				  observation.returnedTime || observation.skeletonRevision || observation.skeleton))
			throw std::invalid_argument("Body observation metadata does not match its provider");
	}
	else if (observation.active || observation.confidence || observation.returnedTime || observation.skeletonRevision ||
			 observation.skeleton || observation.allJointPosesTracked)
		throw std::invalid_argument("Unlocated body observation cannot contain provider metadata");
	if (observation.confidence &&
		(!std::isfinite(*observation.confidence) || *observation.confidence < 0 || *observation.confidence > 1))
		throw std::invalid_argument("Invalid body observation confidence");
	std::array<bool, BodyJointCount> present{};
	for (const auto joint : map)
		present[static_cast<std::size_t>(joint)] = true;
	for (std::size_t index = 0; index < BodyJointCount; ++index)
	{
		auto& joint = observation.joints[index];
		if (joint.has_value() != present[index])
			throw std::invalid_argument("Body observation joints do not match the provider schema");
		if (!joint)
			continue;
		const auto flags = joint->flags;
		if ((flags & ~TrackedPoseFlags) ||
			((flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) && !(flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) ||
			((flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) &&
			 !(flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)) ||
			((!observation.located || observation.active == false) && flags))
			throw std::invalid_argument("Invalid observed body joint flags");
		const auto pose = NormalizedPose(joint->pose);
		if (!pose)
			throw std::invalid_argument("Invalid observed body joint pose");
		joint->pose = *pose;
	}
	if (observation.skeleton)
	{
		for (std::size_t index = 0; index < BodyJointCount; ++index)
		{
			auto& joint = (*observation.skeleton)[index];
			if (joint.has_value() != present[index])
				throw std::invalid_argument("Observed skeleton does not match the provider schema");
			if (!joint)
				continue;
			if (static_cast<std::size_t>(joint->joint) != index ||
				(joint->parent && (static_cast<std::size_t>(*joint->parent) >= BodyJointCount ||
								   !present[static_cast<std::size_t>(*joint->parent)])))
				throw std::invalid_argument("Invalid observed skeleton joint or parent");
			const auto pose = NormalizedPose(joint->bindPose);
			if (!pose)
				throw std::invalid_argument("Invalid observed skeleton rest pose");
			joint->bindPose = *pose;
		}
		for (const auto joint : map)
		{
			auto parent = (*observation.skeleton)[static_cast<std::size_t>(joint)]->parent;
			for (std::size_t depth = 0; parent; ++depth)
			{
				if (depth >= map.size())
					throw std::invalid_argument("Observed skeleton contains a cycle");
				parent = (*observation.skeleton)[static_cast<std::size_t>(*parent)]->parent;
			}
		}
	}
	return observation;
}
BodyObservation RebaseBodyObservation(const BodyObservation& observation, RuntimeTime origin, RuntimeTime target)
{
	auto result = NormalizeBodyObservation(observation);
	if (origin.value.count() <= 0 || target.value.count() <= 0)
		throw std::invalid_argument("Body replay requires positive clock anchors");
	const auto shift = [&](RuntimeTime time)
	{
		// Positive source times and anchors make this subtraction representable.
		const auto offset = time.value.count() - origin.value.count();
		const auto destination = target.value.count();
		if ((offset > 0 && destination > std::numeric_limits<std::int64_t>::max() - offset) ||
			(offset < 0 && destination <= -offset))
			throw std::invalid_argument("Rebased body timestamp is outside the runtime clock range");
		return RuntimeTime{Duration{destination + offset}};
	};
	result.requestedTime = shift(result.requestedTime);
	// Zero returned time is a provider's inactive sentinel, not a clock sample.
	if (result.returnedTime && result.returnedTime->value.count() != 0)
		result.returnedTime = shift(*result.returnedTime);
	return result;
}
BodySample EvaluateBodyObservation(const BodyObservation& observation, bool focused)
{
	BodySample result;
	result.sampleTime = observation.returnedTime.value_or(observation.requestedTime);
	result.skeletonRevision = observation.skeletonRevision.value_or(0);
	result.active = focused && observation.located && observation.active.value_or(true);
	result.confidence = result.active ? observation.confidence.value_or(0) : 0;
	bool anyValid = false;
	for (std::size_t index = 0; index < BodyJointCount; ++index)
	{
		result.skeleton[index] = {static_cast<BodyJoint>(index), {}, IdentityPose()};
		result.joints[index].sampleTime = result.sampleTime;
		if (const auto& joint = observation.joints[index])
		{
			result.joints[index].pose = joint->pose;
			result.joints[index].flags = result.active ? joint->flags : 0;
			anyValid |= (result.joints[index].flags & ValidPoseFlags) != 0;
		}
		if (observation.skeleton && (*observation.skeleton)[index])
			result.skeleton[index] = *(*observation.skeleton)[index];
	}
	result.active &= anyValid;
	if (!result.active)
		result.confidence = 0;
	return result;
}
} // namespace simxr
