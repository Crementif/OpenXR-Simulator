#include "simulation/TrackingSystem.h"
#include "math/Math.h"
#include "../TestSupport.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>

int main()
{
	using namespace simxr;
	TrackingSystem tracking(3);
	TrackingState state;
	Require(!tracking.Sample({Duration{1}}), "Empty history");
	auto first = tracking.Publish({Duration{7000000000000000000LL}}, state);
	state.head.pose.position = {2, 1.9f, 0};
	state.head.pose.orientation = EulerAngles(1.5707963268f, 0, 0);
	state.gaze.yaw = 0.2f;
	auto second = tracking.Publish({Duration{7000000000000000100LL}}, state);
	auto middle = tracking.Sample({Duration{7000000000000000050LL}});
	Require(middle && middle->time.value.count() == 7000000000000000050LL, "Historical nanosecond precision");
	Require(std::abs(middle->head.pose.position.x - 1) < 0.00001f, "Head interpolation");
	auto relativeGaze = RelativePose(middle->gaze.pose, middle->head.pose);
	auto expectedGaze = EulerAngles(0.1f, 0, 0);
	Require(std::abs(relativeGaze.orientation.y - expectedGaze.y) < 0.00001f, "Gaze/head historical coherence");
	auto headJoint = middle->body.joints[static_cast<std::size_t>(BodyJoint::Head)];
	Require(std::abs(headJoint.pose.position.x - middle->head.pose.position.x) < 0.00001f && std::abs(headJoint.pose.position.y - middle->head.pose.position.y) < 0.00001f, "Body/head historical coherence");
	Require(first.head.pose.position.x == 0 && first.body.skeletonRevision == 1, "Published snapshot mutated");
	state.gaze.available = false;
	state.body.controls[static_cast<std::size_t>(BodyControl::LeftFoot)].tracked = false;
	state.trackers[0].connected = false;
	state.controllers[0].connected = false;
	auto disconnected = tracking.Publish({Duration{7000000000000000200LL}}, state);
	Require(disconnected.gaze.flags == 0 && disconnected.trackers[0].pose.flags == 0 && disconnected.controllers[0].pose.flags == 0, "Disconnection activity");
	Require(disconnected.trackers[0].device.identity == first.trackers[0].device.identity, "Disconnection changed identity");
	Require(disconnected.body.joints[static_cast<std::size_t>(BodyJoint::LeftFootAnkle)].flags == ValidPoseFlags, "Body loss is inferred");
	Require(tracking.Sample({Duration{7000000000000000150LL}})->gaze.flags == second.gaze.flags, "Future loss changed earlier state");
	state.body.active = false;
	state.trackers[0].connected = true;
	auto bodyLost = TrackingSystem::Evaluate({Duration{7000000000000000250LL}}, state, BodyModel{});
	Require(!bodyLost.body.active && bodyLost.trackers[0].pose.flags == TrackedPoseFlags, "Body sensor loss disconnected independent tracker");
	state.focused = false;
	auto unfocused = tracking.Publish({Duration{7000000000000000300LL}}, state);
	Require(!unfocused.body.active && unfocused.gaze.flags == 0 && unfocused.controllers[1].pose.flags == 0, "Focus loss");
	Require(!tracking.Sample(first.time), "History budget not enforced");
	Require(tracking.Sample({Duration{7000000000000000400LL}})->time == unfocused.time, "Future query fabricated sensor time");
	bool rejected = false;
	try { tracking.Publish(second.time, state); }
	catch (const std::invalid_argument&) { rejected = true; }
	Require(rejected, "Out-of-order sample accepted");
	rejected = false;
	try { tracking.Publish(unfocused.time, state); }
	catch (const std::invalid_argument&) { rejected = true; }
	Require(rejected, "Duplicate time overwrote immutable sample");
	tracking.Reset();
	state = {};
	tracking.Publish({Duration{10}}, state);
	auto proportions = BodyProportions{};
	proportions.hipWidth = 0.3f;
	tracking.SetBodyProportions(proportions);
	auto changed = tracking.Publish({Duration{20}}, state);
	Require(changed.body.skeletonRevision == 2 && tracking.Sample({Duration{15}})->body.skeletonRevision == 1, "Skeleton configuration leaked backward");
	TrackingSystem wideRange(2);
	TrackingState endpoints;
	endpoints.head.pose.position.x = -2.0e38f;
	wideRange.Publish({Duration{10}}, endpoints);
	endpoints.head.pose.position.x = 2.0e38f;
	wideRange.Publish({Duration{20}}, endpoints);
	const auto midpoint = wideRange.Sample({Duration{15}});
	Require(midpoint && std::isfinite(midpoint->head.pose.position.x) && midpoint->head.pose.position.x == 0,
			"Finite tracking endpoints overflowed during interpolation");

	TrackingSystem rooted(4);
	TrackingState rootState;
	rootState.body.rootPose = XrPosef{EulerAngles(0, 0, 0), {0, 0, 0}};
	rooted.Publish({Duration{10}}, rootState);
	rootState.body.rootPose = XrPosef{EulerAngles(1.0f, 0, 0), {2, 0, -2}};
	rooted.Publish({Duration{20}}, rootState);
	const auto rootMiddle = rooted.Sample({Duration{15}});
	Require(rootMiddle && rootMiddle->input.body.rootPose, "Historical body root missing");
	const auto& root = *rootMiddle->input.body.rootPose;
	Require(std::abs(root.position.x - 1) < .00001f && std::abs(root.position.z + 1) < .00001f &&
		std::abs(root.orientation.y - EulerAngles(.5f, 0, 0).y) < .00001f,
		"Body root did not interpolate with the tracking sample");
	rootState.body.rootPose = root;
	const auto expectedBody = TrackingSystem::Evaluate({Duration{15}}, rootState, BodyModel{});
	for (std::size_t joint = 0; joint < BodyJointCount; ++joint)
	{
		const auto& actual = rootMiddle->body.joints[joint].pose.position;
		const auto& expected = expectedBody.body.joints[joint].pose.position;
		Require(std::abs(actual.x - expected.x) < .00001f && std::abs(actual.z - expected.z) < .00001f,
			"Historical body joints disagree with interpolated root");
	}
	rootState.body.rootPose.reset();
	rooted.Publish({Duration{30}}, rootState);
	Require(rooted.Sample({Duration{25}})->time == RuntimeTime{Duration{20}},
		"Removing the explicit root changed an earlier sample");
	rootState.body.rootPose = XrPosef{EulerAngles(0, 0, 0), {4, 0, 0}};
	rooted.Publish({Duration{40}}, rootState);
	Require(rooted.Sample({Duration{35}})->time == RuntimeTime{Duration{30}},
		"Adding an explicit root changed an earlier sample");

	std::atomic_bool failed = false;
	std::thread reader([&] {
		for (int count = 0; count < 100; ++count)
			if (!tracking.Sample({Duration{1000}})) failed = true;
	});
	for (int count = 21; count < 121; ++count)
		tracking.Publish({Duration{count}}, state);
	reader.join();
	Require(!failed, "Concurrent historical read");
	std::cout << "Tracking tests passed\n";
}
