#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetTracking.h"
#include "math/Math.h"
#include "simulation/BodyModel.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace simxr::tests
{
static void BodyMapping()
{
	using namespace headset;
	for (const auto set :
		 {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA, BodyJointSet::WithoutArmsBD, BodyJointSet::FullBodyBD})
	{
		PhysicalBodySample input;
		input.jointSet = set;
		input.requestedTime = 100;
		input.located = true;
		const auto map = BodyModel::JointMap(set);
		const bool fb = set == BodyJointSet::UpperBodyFB || set == BodyJointSet::FullBodyMETA;
		if (fb)
		{
			input.returnedTime = 90;
			input.active = true;
			input.confidence = .75f;
			input.skeletonRevision = 0;
			input.skeleton.emplace();
		}
		else
			input.allJointPosesTracked = false;
		for (const auto joint : map)
		{
			const auto index = static_cast<std::size_t>(joint);
			input.joints[index] = PhysicalBodyJoint{IdentityPose(), XR_SPACE_LOCATION_ORIENTATION_VALID_BIT};
			input.joints[index]->pose.position = {1, 2, 3};
			if (input.skeleton)
				(*input.skeleton)[index] = SkeletonJoint{joint, {}, IdentityPose()};
		}
		const XrPosef transform{EulerAngles(.5f, .1f, 0), {10, 20, 30}};
		const auto returned = fb ? std::optional{RuntimeTime{Duration{9000}}} : std::nullopt;
		const auto result = MapHeadsetBody(input, {Duration{10000}}, returned, transform);
		HeadsetInputSample batch;
		batch.time = input.requestedTime;
		batch.body = input;
		batch.head.pose = IdentityPose();
		FrameNativeClockMapping clock{
			{1}, 7, NativeClockDomain::MonotonicNanoseconds, 1000000, {Duration{2000000}}, {Duration{2000010}}, 1};
		const auto returnedNative = fb ? std::optional<std::int64_t>{999990} : std::nullopt;
		const auto mapped = MapHeadsetTracking(batch, clock, clock.domain, 1000010, 1000020, transform, 3, 4,
											   std::chrono::milliseconds(1), 1, returnedNative);
		Require(mapped.body && mapped.body->requestedTime == mapped.targetTime && mapped.clockRevision == 7 &&
					(!fb || mapped.body->returnedTime->value.count() == mapped.targetTime.value.count() - 30),
				"Body and head did not share the frame clock mapping and prediction");
		for (unsigned fault = 0; fault < 3; ++fault)
		{
			auto bad = batch;
			auto native = returnedNative;
			if (fault == 0)
				bad.body->requestedTime++;
			if (fault == 1)
			{
				if (fb)
					native.reset();
				else
					native = 0;
			}
			if (fault == 2)
				native = -1;
			bool rejected = false;
			try
			{
				MapHeadsetTracking(bad, clock, clock.domain, 1000010, 1000020, transform, 3, 4,
								   std::chrono::milliseconds(1), 1, native);
			}
			catch (const std::exception&)
			{
				rejected = true;
			}
			Require(rejected, "Mismatched body clock conversion was accepted");
		}

		Require(result.requestedTime.value.count() == 10000 && result.returnedTime == returned &&
					result.jointSet == set && result.confidence == input.confidence && result.located,
				"Body retains converted time and provider metadata without copying native clock values");
		for (std::size_t index = 0; index < BodyJointCount; ++index)
		{
			Require(result.joints[index].has_value() == input.joints[index].has_value(),
					"Missing joints stay unavailable");
			if (!input.joints[index])
				continue;
			const auto expected = Compose(transform, input.joints[index]->pose);
			Require(std::abs(result.joints[index]->pose.position.x - expected.position.x) < .00001f &&
						std::abs(result.joints[index]->pose.orientation.y - expected.orientation.y) < .00001f &&
						result.joints[index]->flags == XR_SPACE_LOCATION_ORIENTATION_VALID_BIT,
					"Body calibration must preserve component validity");
			if (result.skeleton)
				Require((*result.skeleton)[index]->bindPose.position.x == 0,
						"Rest skeleton coordinates must not receive world calibration");
		}
		for (unsigned fault = 0; fault < 7; ++fault)
		{
			auto invalid = result;
			const auto first = static_cast<std::size_t>(map[0]);
			if (fault == 0)
				invalid.joints[first].reset();
			if (fault == 1)
				invalid.joints[first]->flags = XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
			if (fault == 2)
				invalid.joints[first]->pose.orientation = {};
			if (fault == 3)
				invalid.requestedTime = {};
			if (fault == 4)
				invalid.located = false;
			if (fault == 5)
				invalid.confidence = 2.f;
			if (fault == 6)
			{
				if (fb)
					(*invalid.skeleton)[first]->parent = map[0];
				else
					invalid.active = true;
			}
			bool rejected = false;
			try
			{
				NormalizeBodyObservation(invalid);
			}
			catch (const std::invalid_argument&)
			{
				rejected = true;
			}
			Require(rejected, "Malformed body observation must reject before publication");
		}
		input.located = false;
		input.active.reset();
		input.confidence.reset();
		input.returnedTime.reset();
		input.skeletonRevision.reset();
		input.skeleton.reset();
		input.allJointPosesTracked.reset();
		for (auto& joint : input.joints)
			if (joint)
				joint->flags = 0;
		const auto suppressed = MapHeadsetBody(input, {Duration{10000}}, {}, transform);
		Require(!suppressed.located && !suppressed.returnedTime && !suppressed.skeleton,
				"Unlocated body observations remain explicit after calibration");
	}
}
static void RestartAlignment()
{
	using namespace headset;
	HeadsetInputSample before;
	before.head.pose = {EulerAngles(.3f, 0, 0), {1, 1.2f, -2}};
	before.head.locationFlags = ValidPoseFlags;
	before.space.initialFromCurrent = {EulerAngles(.4f, 0, 0), {2, 0, 1}};
	before.space.revision = 4;
	const XrPosef calibration{EulerAngles(.2f, 0, 0), {3, 2, -1}};
	const auto alignment = HeadsetRestartAlignment(before, calibration, 2);
	const auto expected = Compose(calibration, Compose(before.space.initialFromCurrent, before.head.pose));
	Require(std::abs(alignment.worldHead.position.x - expected.position.x) < .00001f &&
				std::abs(*alignment.localEyeHeight - 1.2f) < .00001f,
			"Restart lost recenter transform or current floor-relative height");
	HeadsetInputSample after;
	after.referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	after.head.pose = {EulerAngles(-.5f, 0, 0), {-3, .2f, 4}};
	after.head.locationFlags = ValidPoseFlags;
	const auto resumed = Compose(AlignHeadset(after, alignment), after.head.pose);
	Require(std::abs(resumed.position.x - expected.position.x) < .00001f &&
				std::abs(resumed.position.y - expected.position.y) < .00001f &&
				std::abs(resumed.position.z - expected.position.z) < .00001f &&
				std::abs(resumed.orientation.y - expected.orientation.y) < .00001f,
			"Recreated LOCAL session teleported the calibrated player");
	before.head.locationFlags = 0;
	bool rejected = false;
	try
	{
		(void)HeadsetRestartAlignment(before, calibration, 2);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Restart fabricated alignment from an unavailable head");
}
static void Alignment()
{
	using namespace headset;
	HeadsetInputSample input;
	input.focused = input.controlsReady = true;
	input.referenceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	input.head.locationFlags = TrackedPoseFlags;
	input.head.pose = {EulerAngles(.4f, .2f, 0), {1, 1.7f, -2}};
	HeadsetAlignment target;
	target.worldHead = {EulerAngles(1.2f, 0, 0), {5, 999, -3}};
	target.worldFloor = 2;
	const auto mapped = Compose(AlignHeadset(input, target), input.head.pose);
	const auto forward = Rotate(mapped.orientation, {0, 0, -1});
	Require(std::abs(mapped.position.x - 5) < .00001f && std::abs(mapped.position.z + 3) < .00001f &&
				std::abs(mapped.position.y - 3.7f) < .00001f &&
				std::abs(std::atan2(-forward.x, -forward.z) - 1.2f) < .00001f,
			"Initial alignment matches horizontal player position/yaw and preserves floor-relative head height");
	input.space.initialFromCurrent = {EulerAngles(.2f, 0, 0), {2, 0, 1}};
	input.space.revision = 2;
	const auto adjusted =
		Compose(Compose(AlignHeadset(input, target), input.space.initialFromCurrent), input.head.pose);
	Require(std::abs(adjusted.position.x - mapped.position.x) < .00001f &&
				std::abs(adjusted.position.z - mapped.position.z) < .00001f,
			"Alignment is expressed in the initial origin even after a physical recenter");
	input.referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL;
	bool rejected = false;
	try
	{
		AlignHeadset(input, target);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "LOCAL cannot fabricate floor height");
	target.localEyeHeight = 1.5f;
	const auto local = Compose(Compose(AlignHeadset(input, target), input.space.initialFromCurrent), input.head.pose);
	Require(std::abs(local.position.y - 3.5f) < .00001f, "Explicit LOCAL eye height establishes floor calibration");
	input.head.locationFlags = 0;
	rejected = false;
	try
	{
		AlignHeadset(input, target);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Unavailable head pose cannot establish alignment");
}
static void Mapping()
{
	using namespace headset;
	FrameNativeClockMapping clock{
		{1}, 7, NativeClockDomain::MonotonicNanoseconds, 1000000, {Duration{2000000}}, {Duration{2000010}}, 1};
	HeadsetInputSample input;
	input.focused = input.controlsReady = true;
	input.viewsLocated = true;
	for (std::size_t eye = 0; eye < 2; ++eye)
	{
		input.eyes[eye].location.pose = {EulerAngles(eye ? -.15f : .15f, 0, 0), {eye ? .04f : -.03f, 1.6f, 0}};
		input.eyes[eye].location.locationFlags = eye ? 0 : TrackedPoseFlags;
		input.eyes[eye].fov = {-.7f, .8f, .6f, -.9f};
	}
	input.head.pose = IdentityPose();
	input.head.locationFlags = TrackedPoseFlags;
	input.hands[0].aim.pose = IdentityPose();
	input.hands[0].aim.pose.position.x = 2;
	input.hands[0].aim.locationFlags = TrackedPoseFlags;
	input.hands[0].select.isActive = true;
	input.hands[0].select.currentState = true;
	auto calibration = IdentityPose();
	calibration.position.x = 10;
	const auto map = [&]
	{
		return MapHeadsetTracking(input, clock, NativeClockDomain::MonotonicNanoseconds, 1000010, 1000020, calibration,
								  3, 4);
	};
	Require(!map().trackers, "Absent provider became an empty physical inventory");
	input.trackers.emplace();
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
	{
		PhysicalTrackerSample physical;
		physical.persistentPath = "/device/" + std::to_string(index);
		physical.location.pose = IdentityPose();
		physical.location.pose.position.x = static_cast<float>(index);
		physical.location.locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT;
		input.trackers->push_back(physical);
	}
	const auto trackers = map();
	Require(trackers.trackers->size() == 64 && trackers.trackers->back().poseOverride->position.x == 73 &&
				trackers.trackers->back().poseFlags == XR_SPACE_LOCATION_POSITION_VALID_BIT &&
				!trackers.trackers->back().tracked && trackers.trackers->back().sourcePath.View() == "htcx:/device/63",
			"Physical tracker calibration, identity or partial validity changed");
	std::reverse(input.trackers->begin(), input.trackers->end());
	input.trackers->front().role = TrackerRole::Waist;
	Require(map().trackers->front().identity == trackers.trackers->back().identity,
			"Enumeration order or role reassignment changed physical handle");
	input.trackers->back().persistentPath = input.trackers->front().persistentPath;
	bool duplicate = false;
	try
	{
		(void)map();
	}
	catch (const std::invalid_argument&)
	{
		duplicate = true;
	}
	Require(duplicate, "Duplicate physical identities were published");
	input.trackers->clear();
	Require(map().trackers && map().trackers->empty(), "Empty provider inventory was omitted");
	input.trackers.reset();
	const auto sample = map();
	Require(sample.controlsActive, "Focused, recovered controls remain active");
	input.focused = false;
	Require(!map().controlsActive, "Physical focus loss crosses the tracking boundary");
	input.focused = true;
	input.controlsReady = false;
	Require(!map().controlsActive, "Neutral recovery keeps live controls suspended");
	input.controlsReady = true;
	Require(sample.clockRevision == 7 && sample.observedTime.value.count() == 2000015 &&
				sample.targetTime.value.count() == 2000025,
			"Native clock mapping preserves epoch offset and prediction interval");
	Require(sample.head.pose.position.x == 10 && sample.hands[0].aim->pose.position.x == 12 &&
				sample.hands[0].pose.flags == 0 && sample.hands[0].aim->flags == TrackedPoseFlags,
			"Calibration is shared across head and aim while lost grip remains invalid");
	Require(sample.eyeViews && sample.eyeViews->spaceRevision == 1 &&
				std::abs(sample.eyeViews->poses[0].pose.position.x - 9.97f) < .00001f &&
				sample.eyeViews->poses[0].pose.orientation.y > 0 && sample.eyeViews->poses[1].flags == 0 &&
				sample.eyeViews->fovs[1].angleDown == -.9f && sample.eyeViews->poses[0].sampleTime == sample.targetTime,
			"Calibration preserves cant, asymmetric FOV, eye validity and prediction time");
	Require(sample.hands[0].pose.pose.orientation.w == 1 && sample.controls[0].trigger == 1 &&
				!sample.controls[0].primary && !sample.hands[1].connected,
			"Unavailable poses use finite placeholders and simple selection maps to trigger");
	input.space.initialFromCurrent.position.x = 2;
	input.space.revision = 2;
	input.head.pose.position.x = -2;
	input.hands[0].aim.pose.position.x -= 2;
	input.eyes[0].location.pose.position.x -= 2;
	const auto recentered = map();
	Require(recentered.head.pose.position.x == sample.head.pose.position.x &&
				recentered.hands[0].aim->pose.position.x == sample.hands[0].aim->pose.position.x &&
				std::abs(recentered.eyeViews->poses[0].pose.position.x - sample.eyeViews->poses[0].pose.position.x) <
					.00001f &&
				recentered.eyeViews->spaceRevision == 2,
			"A runtime recenter preserves calibrated world head, aim and eye positions while revising the space");
	input.space = {};
	input.head.pose.position.x = 0;
	input.hands[0].aim.pose.position.x += 2;
	input.eyes[0].location.pose.position.x += 2;
	input.hands[0].trigger.isActive = true;
	input.hands[0].trigger.currentState = .25f;
	Require(map().controls[0].trigger == .25f, "Analog trigger remains independent of boolean selection");
	input.hands[0].grip.pose.position.x = 3;
	input.hands[0].grip.locationFlags = TrackedPoseFlags;
	const auto partial = map();
	Require(partial.hands[0].pose.flags ==
					(XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT) &&
				partial.hands[0].pose.pose.position.x == 13,
			"Invalid orientation does not discard a valid tracked position");
	for (int fault = 0; fault < 4; ++fault)
	{
		auto invalid = clock;
		if (fault == 0)
			invalid.domain = NativeClockDomain::PerformanceCounterNanoseconds;
		if (fault == 1)
			invalid.runtimeAfter = {Duration{9000000}};
		if (fault == 2)
			invalid.runtimeAfter = invalid.runtimeBefore = {Duration{std::numeric_limits<std::int64_t>::max()}};
		if (fault == 3)
			invalid.revision = 0;
		bool rejected = false;
		try
		{
			MapHeadsetTracking(input, invalid, NativeClockDomain::MonotonicNanoseconds, 1000010, 1000020, calibration,
							   3, 4);
		}
		catch (const std::exception&)
		{
			rejected = true;
		}
		Require(rejected, "Reject domain mismatch, excessive uncertainty, overflow and missing revision");
	}
}
} // namespace simxr::tests
int main()
{
	try
	{
		simxr::tests::BodyMapping();
		simxr::tests::RestartAlignment();
		simxr::tests::Alignment();
		simxr::tests::Mapping();
		std::cout << "Headset tracking mapping checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
