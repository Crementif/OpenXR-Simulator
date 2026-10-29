#include "headset/HeadsetTracking.h"
#include "Pch.h"
#include "math/Math.h"
#include "simulation/ViewConfiguration.h"

namespace simxr::headset
{
XrPosef AlignHeadset(const HeadsetInputSample& input, const HeadsetAlignment& alignment)
{
	const auto physicalHead = NormalizedPose(input.head.pose);
	const auto target = NormalizedPose(alignment.worldHead);
	const auto origin = NormalizedPose(input.space.initialFromCurrent);
	if (!physicalHead || !target || !origin || !input.space.revision || !std::isfinite(alignment.worldFloor) ||
		(input.head.locationFlags & ValidPoseFlags) != ValidPoseFlags)
		throw std::invalid_argument("Headset alignment requires valid head poses and floor calibration");
	const auto yaw = [](const XrPosef& pose) {
		const auto forward = Rotate(pose.orientation, {0, 0, -1});
		if (std::hypot(forward.x, forward.z) < .0001f)
			throw std::invalid_argument("Look horizontally to establish headset alignment");
		return std::atan2(-forward.x, -forward.z);
	};
	XrPosef worldFromCurrent = IdentityPose();
	worldFromCurrent.orientation = EulerAngles(yaw(*target) - yaw(*physicalHead), 0, 0);
	const auto rotated = Rotate(worldFromCurrent.orientation, physicalHead->position);
	worldFromCurrent.position = {target->position.x - rotated.x, alignment.worldFloor, target->position.z - rotated.z};
	if (input.referenceType == XR_REFERENCE_SPACE_TYPE_LOCAL)
	{
		if (!alignment.localEyeHeight || !std::isfinite(*alignment.localEyeHeight) || *alignment.localEyeHeight <= 0)
			throw std::invalid_argument("LOCAL tracking requires an explicit eye height above the floor");
		worldFromCurrent.position.y += *alignment.localEyeHeight - physicalHead->position.y;
	}
	else if (input.referenceType != XR_REFERENCE_SPACE_TYPE_STAGE &&
			 input.referenceType != XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR_EXT)
		throw std::invalid_argument("Unsupported physical alignment origin");
	const auto result = NormalizedPose(Compose(worldFromCurrent, RelativePose(IdentityPose(), *origin)));
	if (!result)
		throw std::invalid_argument("Headset alignment exceeds the finite coordinate range");
	return *result;
}

HeadsetAlignment HeadsetRestartAlignment(const HeadsetInputSample& input, XrPosef worldFromInitial, float worldFloor)
{
	const auto transform = NormalizedPose(worldFromInitial);
	const auto origin = NormalizedPose(input.space.initialFromCurrent);
	const auto head = NormalizedPose(input.head.pose);
	if (!transform || !origin || !head || !input.space.revision || !std::isfinite(worldFloor) ||
		(input.head.locationFlags & ValidPoseFlags) != ValidPoseFlags)
		throw std::invalid_argument("Session rebuild requires a valid calibrated head observation");
	const auto world = NormalizedPose(Compose(*transform, Compose(*origin, *head)));
	if (!world || world->position.y <= worldFloor)
		throw std::invalid_argument("Session rebuild requires a head observation above the calibrated floor");
	HeadsetAlignment alignment;
	alignment.worldHead = *world;
	alignment.worldFloor = worldFloor;
	alignment.localEyeHeight = world->position.y - worldFloor;
	return alignment;
}

BodyObservation MapHeadsetBody(const PhysicalBodySample& input, RuntimeTime requested,
							   std::optional<RuntimeTime> returned, XrPosef worldFromBase)
{
	const auto transform = NormalizedPose(worldFromBase);
	if (!transform || returned.has_value() != input.returnedTime.has_value())
		throw std::invalid_argument("Body calibration or converted timestamp is unavailable");
	BodyObservation result;
	result.jointSet = input.jointSet;
	result.requestedTime = requested;
	result.returnedTime = returned;
	result.located = input.located;
	result.active = input.active;
	result.allJointPosesTracked = input.allJointPosesTracked;
	result.confidence = input.confidence;
	result.skeletonRevision = input.skeletonRevision;
	result.skeleton = input.skeleton;
	for (std::size_t index = 0; index < input.joints.size(); ++index)
		if (const auto& source = input.joints[index])
		{
			ObservedBodyJoint joint{Compose(*transform, source->pose), source->flags};
			joint.flags = FinitePoseFlags(joint.pose, joint.flags);
			result.joints[index] = joint;
		}
	return NormalizeBodyObservation(std::move(result));
}

static RuntimeTime MapNativeTime(const FrameNativeClockMapping& clock, std::int64_t native)
{
	if (native <= 0)
		throw std::invalid_argument("Invalid native sample timestamp");
	const auto anchor =
		clock.runtimeBefore.value.count() + (clock.runtimeAfter.value.count() - clock.runtimeBefore.value.count()) / 2;
	const auto offset = native - clock.nativeNanoseconds;
	if ((offset > 0 && anchor > std::numeric_limits<std::int64_t>::max() - offset) || (offset < 0 && offset <= -anchor))
		throw std::overflow_error("Mapped physical timestamp is outside runtime range");
	return {Duration{anchor + offset}};
}

static PoseSample MapPose(const XrSpaceLocation& location, RuntimeTime target, XrPosef transform)
{
	PoseSample result;
	result.flags = FinitePoseFlags(location.pose, location.locationFlags) & TrackedPoseFlags;
	// OpenXR leaves unavailable components unspecified, including zero quaternions.
	// Keep their validity clear while using finite placeholders for transport.
	if (result.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
		result.pose.orientation = location.pose.orientation;
	else
		result.flags &= ~XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	if (result.flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
		result.pose.position = location.pose.position;
	else
		result.flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	const auto normalized = NormalizedPose(result.pose);
	if (!normalized)
	{
		result.flags &= ~(XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
		result.pose.orientation = IdentityPose().orientation;
	}
	else
		result.pose = *normalized;
	result.pose = Compose(transform, result.pose);
	result.sampleTime = target;
	return result;
}

LiveInputSample MapHeadsetTracking(const HeadsetInputSample& input, const FrameNativeClockMapping& clock,
								   NativeClockDomain domain, std::int64_t observed, std::int64_t predicted,
								   XrPosef worldFromPhysical, std::uint64_t attachment, std::uint64_t sequence,
								   Duration maximumUncertainty, std::uint64_t spaceRevision,
								   std::optional<std::int64_t> bodyReturned)
{
	const auto initial = NormalizedPose(worldFromPhysical);
	const auto origin = NormalizedPose(input.space.initialFromCurrent);
	const auto transform = initial && origin ? NormalizedPose(Compose(*initial, *origin)) : std::nullopt;
	if (!transform || !spaceRevision || !input.space.revision ||
		input.space.revision - 1 > std::numeric_limits<std::uint64_t>::max() - spaceRevision || !attachment ||
		!sequence || !clock.generation.value || !clock.revision ||
		(domain != NativeClockDomain::MonotonicNanoseconds &&
		 domain != NativeClockDomain::PerformanceCounterNanoseconds) ||
		clock.domain != domain || clock.nativeNanoseconds <= 0 || clock.resolutionNanoseconds <= 0 ||
		clock.runtimeBefore.value.count() <= 0 || clock.runtimeAfter < clock.runtimeBefore ||
		maximumUncertainty.count() <= 0 || predicted < observed)
		throw std::invalid_argument("Invalid physical tracking clock or calibration");
	const auto span = clock.runtimeAfter.value.count() - clock.runtimeBefore.value.count();
	const auto radius = span / 2 + span % 2;
	if (radius > maximumUncertainty.count() || clock.resolutionNanoseconds > maximumUncertainty.count() - radius)
		throw std::runtime_error("Physical tracking clock uncertainty exceeds its budget");
	LiveInputSample sample;
	sample.controlsActive = input.focused && input.controlsReady;
	sample.connection = attachment;
	sample.sequence = sequence;
	sample.clockRevision = clock.revision;
	sample.observedTime = MapNativeTime(clock, observed);
	sample.targetTime = MapNativeTime(clock, predicted);
	if (input.body)
	{
		if (input.body->requestedTime != input.time ||
			input.body->returnedTime.has_value() != bodyReturned.has_value() ||
			(bodyReturned && (*bodyReturned < 0 || ((*bodyReturned == 0) != (*input.body->returnedTime == 0)))))
			throw std::invalid_argument("Body clock conversion does not match its tracking batch");
		std::optional<RuntimeTime> returned;
		if (bodyReturned)
			returned = *bodyReturned == 0 ? RuntimeTime{} : MapNativeTime(clock, *bodyReturned);
		sample.body = MapHeadsetBody(*input.body, sample.targetTime, returned, *transform);
	}
	else if (bodyReturned)
		throw std::invalid_argument("Body sample time has no provider observation");
	sample.head = MapPose(input.head, sample.targetTime, *transform);
	if (input.viewsLocated)
	{
		sample.eyeViews.emplace();
		sample.eyeViews->spaceRevision = spaceRevision + input.space.revision - 1;
		for (std::size_t eye = 0; eye < 2; ++eye)
		{
			if (!ValidViewFieldOfView(input.eyes[eye].fov))
				throw std::invalid_argument("Physical runtime returned invalid eye FOV");
			sample.eyeViews->poses[eye] = MapPose(input.eyes[eye].location, sample.targetTime, *transform);
			sample.eyeViews->fovs[eye] = input.eyes[eye].fov;
		}
	}
	if (input.trackers)
	{
		if (input.trackers->size() > MaxTrackerDevices)
			throw std::invalid_argument("Physical tracker inventory exceeds capacity");
		sample.trackers.emplace();
		sample.trackers->reserve(input.trackers->size());
		for (const auto& physical : *input.trackers)
		{
			TrackerDevice tracker{};
			tracker.sourcePath = TrackerSourcePath{"htcx:" + physical.persistentPath};
			// Stable FNV-1a over the namespaced path, never over an instance-local XrPath.
			std::uint64_t identity = 14695981039346656037ULL;
			for (const unsigned char character : tracker.sourcePath.View())
			{
				identity ^= character;
				identity *= 1099511628211ULL;
			}
			tracker.identity.value = identity | (1ULL << 63);
			for (const auto& prior : *sample.trackers)
				if (prior.identity == tracker.identity)
					throw std::invalid_argument("Duplicate or colliding physical tracker identities");
			tracker.role = physical.role;
			const auto pose = MapPose(physical.location, sample.targetTime, *transform);
			tracker.poseOverride = pose.pose;
			tracker.poseFlags = pose.flags;
			tracker.tracked =
				(pose.flags & (XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)) !=
				0;
			tracker.connected = true;
			sample.trackers->push_back(tracker);
		}
	}
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		const auto& source = input.hands[hand];
		auto& pose = sample.hands[hand];
		pose.pose = MapPose(source.grip, sample.targetTime, *transform);
		pose.aim = MapPose(source.aim, sample.targetTime, *transform);
		pose.connected = source.controls.connected || source.select.isActive || source.trigger.isActive ||
						 pose.pose.flags || pose.aim->flags;
		auto& controls = sample.controls[hand];
		controls = source.controls;
		controls.connected = pose.connected;
		controls.trigger = source.trigger.isActive
							   ? source.trigger.currentState
							   : (source.select.isActive && source.select.currentState ? 1.0f : 0.0f);
	}
	InputSnapshot checked;
	checked.controllers = sample.controls;
	if (XR_FAILED(InputState::Validate(checked)))
		throw std::invalid_argument("Physical controller returned invalid controls");
	return sample;
}
} // namespace simxr::headset
