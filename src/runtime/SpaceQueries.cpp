#include "runtime/SpaceQueries.h"
#include "Pch.h"
#include "math/Math.h"
#include "runtime/validation/StructureChain.h"

namespace simxr
{
XrResult LocateSpace(SpaceSystem& spaces, XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location,
					 const EnabledExtensions* enabled)
{
	if (!location || location->type != XR_TYPE_SPACE_LOCATION)
		return XR_ERROR_VALIDATION_FAILURE;
	StructureChain chain(location->next);
	const bool gazeEnabled = enabled && enabled->Contains(XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME);
	const std::array allKnown{XR_TYPE_SPACE_VELOCITY, XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT};
	const auto known = std::span(allKnown).first(gazeEnabled ? 2 : 1);
	auto result = chain.Validate(known);
	if (XR_FAILED(result))
		return result;
	SpaceLocation sample;
	result = spaces.Locate(space, base, {Duration{time}}, sample);
	if (XR_FAILED(result))
		return result;
	location->pose = sample.pose.pose;
	location->locationFlags = sample.pose.flags;
	if (auto velocity = const_cast<XrSpaceVelocity*>(chain.Find<XrSpaceVelocity>(XR_TYPE_SPACE_VELOCITY)))
	{
		velocity->velocityFlags = sample.velocityFlags;
		velocity->linearVelocity = sample.linearVelocity;
		velocity->angularVelocity = sample.angularVelocity;
	}
	if (gazeEnabled)
		if (auto gaze = const_cast<XrEyeGazeSampleTimeEXT*>(
				chain.Find<XrEyeGazeSampleTimeEXT>(XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT)))
			gaze->time = sample.pose.eyeGazeSampleTime.value_or(RuntimeTime{}).value.count();
	return XR_SUCCESS;
}

XrResult LocateViews(SpaceSystem& spaces, const ViewConfiguration& configuration, const TrackingSnapshot& snapshot,
					 const XrViewLocateInfo& info, XrViewState* state, std::uint32_t capacity, std::uint32_t* count,
					 XrView* views, const EnabledExtensions* enabled, ViewQuerySnapshot* query)
{
	if (info.type != XR_TYPE_VIEW_LOCATE_INFO || !state || state->type != XR_TYPE_VIEW_STATE || !count ||
		(capacity && !views))
		return XR_ERROR_VALIDATION_FAILURE;
	const bool foveationEnabled = enabled && enabled->Contains(XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME);
	const std::array known{XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO};
	const StructureChain chain(info.next);
	if (XR_FAILED(chain.Validate(std::span(known).first(foveationEnabled ? 1 : 0))) ||
		XR_FAILED(StructureChain(state->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	bool foveated = false;
	if (foveationEnabled)
		if (const auto request = chain.Find<XrViewLocateFoveatedRenderingVARJO>(known[0]))
		{
			if (request->foveatedRenderingActive > XR_TRUE)
				return XR_ERROR_VALIDATION_FAILURE;
			foveated = request->foveatedRenderingActive == XR_TRUE;
		}
	PoseSample base;
	auto result = spaces.WorldPose(info.space, {Duration{info.displayTime}}, base);
	if (XR_FAILED(result))
		return result;
	const auto samples = configuration.Locate(snapshot, foveated);
	*count = static_cast<std::uint32_t>(samples.views.size());
	if (capacity && capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	for (std::uint32_t index = 0; index < (capacity ? *count : 0); ++index)
		if (views[index].type != XR_TYPE_VIEW || XR_FAILED(StructureChain(views[index].next).Validate({})))
			return XR_ERROR_VALIDATION_FAILURE;
	auto flags = base.flags;
	for (const auto& sample : samples.views)
		flags &= FinitePoseFlags(RelativePose(sample.pose.pose, base.pose), sample.pose.flags);
	if (!(base.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		flags &= ~XR_SPACE_LOCATION_POSITION_VALID_BIT;
	if (!(base.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
		flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	state->viewStateFlags = 0;
	if (flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
		state->viewStateFlags |= XR_VIEW_STATE_ORIENTATION_VALID_BIT;
	if (flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)
		state->viewStateFlags |= XR_VIEW_STATE_ORIENTATION_TRACKED_BIT;
	if (flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
		state->viewStateFlags |= XR_VIEW_STATE_POSITION_VALID_BIT;
	if (flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT)
		state->viewStateFlags |= XR_VIEW_STATE_POSITION_TRACKED_BIT;
	for (std::uint32_t index = 0; index < (capacity ? *count : 0); ++index)
	{
		views[index].pose = RelativePose(samples.views[index].pose.pose, base.pose);
		views[index].fov = samples.views[index].fov;
	}
	if (query && capacity)
	{
		*query = {{Duration{info.displayTime}}, snapshot.time, foveated, samples.gazeActive};
		for (const auto& view : samples.views)
			query->fieldsOfView.push_back(view.fov);
	}
	return XR_SUCCESS;
}
} // namespace simxr
