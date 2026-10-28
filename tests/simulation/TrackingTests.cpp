#include "../TestSupport.h"
#include "math/Math.h"
#include "simulation/CommandState.h"
#include "simulation/TrackingSystem.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <thread>

static void CheckTrackerSources()
{
	using namespace simxr;
	for (const auto& invalid : {std::string("missing_namespace"), std::string("htcx:/"),
		std::string("htcx:/bad space"), std::string("htcx:/bad\0path", 14), std::string("htcx:/") + std::string(282, 'a')})
	{
		bool rejected = false;
		try { (void)TrackerSourcePath{invalid}; }
		catch (const std::invalid_argument&) { rejected = true; }
		Require(rejected, "Malformed physical source path accepted");
	}
	TrackingState state;
	state.trackers[0].sourcePath = TrackerSourcePath{"htcx:/device/one"};
	TrackingSystem history;
	history.Publish({Duration{10}}, state);
	state.trackers[1].sourcePath = state.trackers[0].sourcePath;
	bool rejected = false;
	try { history.Publish({Duration{20}}, state); }
	catch (const std::invalid_argument&) { rejected = true; }
	Require(rejected && history.Sample({Duration{20}})->time == RuntimeTime{Duration{10}},
		"Duplicate physical source changed published history");
	state.trackers[1].sourcePath = {};
	state.trackers[0].sourcePath = TrackerSourcePath{"htcx:/device/replacement"};
	history.Publish({Duration{20}}, state);
	Require(history.Sample({Duration{15}})->time == RuntimeTime{Duration{10}},
		"Source replacement interpolated unrelated devices");
}
static void CheckTrackerValidity()
{
	using namespace simxr;
	TrackingState state;
	TrackingSystem history;
	state.trackers[0].poseOverride = XrPosef{{0, 0, 0, 1}, {1, 2, 3}};
	const auto orientation = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	state.trackers[0].poseFlags = orientation;
	const auto first = history.Publish({Duration{10}}, state);
	Require(first.trackers[0].pose.flags == orientation && first.trackers[1].pose.flags == TrackedPoseFlags,
			"Partial tracker validity was fabricated or leaked to another device");
	state.trackers[0].poseFlags = 0;
	history.Publish({Duration{20}}, state);
	Require(history.Sample({Duration{15}})->time == RuntimeTime{Duration{10}} &&
				history.Sample({Duration{20}})->trackers[0].pose.flags == 0,
			"Tracker validity loss was interpolated or ignored");
	state.trackers[0].poseFlags = XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	bool rejected = false;
	try
	{
		history.Publish({Duration{30}}, state);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && history.Sample({Duration{30}})->time == RuntimeTime{Duration{20}},
			"Invalid tracker flags changed published history");
	state.trackers[0].poseFlags = TrackedPoseFlags;
	state.trackers[0].tracked = false;
	Require(history.Publish({Duration{40}}, state).trackers[0].pose.flags == ValidPoseFlags,
			"Tracker tracked toggle no longer clears tracking flags");
	state.trackers[0].connected = false;
	Require(history.Publish({Duration{50}}, state).trackers[0].pose.flags == 0,
			"Disconnected tracker retained validity");
}
static void CheckAuthoredTrackers()
{
	using namespace simxr;
	CommandConfiguration configuration;
	configuration.tracking.trackers.clear();
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
	{
		TrackerDevice device{{100 + index}, TrackerRole::Unassigned};
		device.poseOverride = XrPosef{{0, 0, 0, 1}, {static_cast<float>(index), 1, 0}};
		configuration.tracking.trackers.push_back(device);
	}
	TrackingSystem tracking;
	const auto first = tracking.Publish({Duration{10}}, configuration.tracking);
	Require(first.trackers.size() == MaxTrackerDevices && first.input.trackers.size() == MaxTrackerDevices,
			"Authored trackers were truncated during publication");
	for (auto& device : configuration.tracking.trackers)
		device.poseOverride->position.x += 2;
	tracking.Publish({Duration{20}}, configuration.tracking);
	const auto middle = tracking.Sample({Duration{15}});
	Require(middle && middle->trackers.size() == MaxTrackerDevices &&
				middle->trackers.back().device.identity.value == 163 &&
				middle->trackers.back().pose.pose.position.x == 64,
			"Expanded authored tracker motion did not interpolate");
	CommandState commands(configuration);
	auto moved = configuration.tracking.trackers.back();
	moved.poseOverride->position.x = 80;
	CommandTransaction transaction;
	transaction.source = {CommandSourceKind::Manual, 1};
	transaction.commands = {TakeControl{ControlChannel::Trackers}, SetTracker{63, moved}};
	Require(commands.Apply(transaction, configuration, {Duration{21}}) == CommandResult::Applied &&
				commands.Merge(configuration).tracking.trackers.back().poseOverride->position.x == 80,
			"Legacy slot command could not edit an expanded initial inventory");
	auto duplicate = moved;
	duplicate.identity = configuration.tracking.trackers.front().identity;
	transaction.commands = {SetTracker{63, duplicate}};
	Require(commands.Apply(transaction, configuration, {Duration{22}}) == CommandResult::Invalid &&
				commands.Merge(configuration).tracking.trackers.back().identity.value == 163,
			"Duplicate tracker identity changed command state");
	const auto origin = commands.RecordingOrigin(configuration, {Duration{23}});
	Require(origin.configuration.tracking.trackers.size() == MaxTrackerDevices &&
				origin.configuration.tracking.trackers.back().poseOverride->position.x == 80,
			"Recording origin truncated the authored inventory");
	auto replacement = configuration.tracking.trackers;
	std::reverse(replacement.begin(), replacement.end());
	transaction.commands = {SetTrackers{replacement}};
	Require(commands.Apply(transaction, configuration, {Duration{24}}) == CommandResult::Applied &&
				commands.Merge(configuration).tracking.trackers.front().identity.value == 163,
			"Atomic collection reorder failed");
	replacement.back().identity = replacement.front().identity;
	transaction.commands = {SetTrackers{replacement}};
	Require(commands.Apply(transaction, configuration, {Duration{25}}) == CommandResult::Invalid &&
				commands.Merge(configuration).tracking.trackers.back().identity.value == 100,
			"Invalid replacement partially changed tracker inventory");
	transaction.commands = {SetTrackers{}};
	Require(commands.Apply(transaction, configuration, {Duration{26}}) == CommandResult::Applied &&
				commands.Merge(configuration).tracking.trackers.empty(),
			"Collection removal retained trackers");
	transaction.commands = {SetTrackers{configuration.tracking.trackers}};
	Require(commands.Apply(transaction, configuration, {Duration{27}}) == CommandResult::Applied &&
				commands.Merge(configuration).tracking.trackers.size() == MaxTrackerDevices,
			"Collection could not restore removed devices");
	transaction.source = {CommandSourceKind::Manual, 2};
	transaction.commands = {SetTrackers{}};
	Require(commands.Apply(transaction, configuration, {Duration{28}}) != CommandResult::Applied &&
				commands.Merge(configuration).tracking.trackers.size() == MaxTrackerDevices,
			"Collection replacement bypassed ownership");
	std::reverse(configuration.tracking.trackers.begin(), configuration.tracking.trackers.end());
	tracking.Publish({Duration{30}}, configuration.tracking);
	Require(tracking.Sample({Duration{25}})->time == RuntimeTime{Duration{20}} &&
				tracking.Sample({Duration{30}})->trackers.front().device.identity.value == 163,
			"Tracker reordering interpolated between unrelated identities");
	auto invalid = configuration.tracking;
	invalid.trackers.push_back({{900}, TrackerRole::Unassigned});
	bool rejected = false;
	try
	{
		tracking.Publish({Duration{40}}, invalid);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && tracking.Sample({Duration{40}})->trackers.size() == MaxTrackerDevices,
			"Over-capacity authored state changed history");
	configuration.tracking.trackers.clear();
	const auto empty = tracking.Publish({Duration{50}}, configuration.tracking);
	Require(empty.trackers.empty() && empty.input.trackers.empty(),
			"Empty authored inventory retained default devices");
	tracking.Publish({Duration{60}}, configuration.tracking);
	Require(tracking.Sample({Duration{55}})->trackers.empty(), "Empty authored history reconstructed default devices");
}
int main()
{
	{
		simxr::TrackingState shared;
		shared.trackers[1].role = shared.trackers[0].role;
		const auto snapshot = simxr::TrackingSystem::Evaluate({simxr::Duration{1}}, shared, simxr::BodyModel{});
		Require(snapshot.trackers.size() == 4 && snapshot.trackers[0].device.role == snapshot.trackers[1].device.role &&
				snapshot.trackers[0].device.identity != snapshot.trackers[1].device.identity,
				"Shared tracker role collapsed or rejected independent identities");
	}
	CheckTrackerSources();
	CheckTrackerValidity();
	CheckAuthoredTrackers();
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
	Require(std::abs(headJoint.pose.position.x - middle->head.pose.position.x) < 0.00001f &&
				std::abs(headJoint.pose.position.y - middle->head.pose.position.y) < 0.00001f,
			"Body/head historical coherence");
	Require(first.head.pose.position.x == 0 && first.body.skeletonRevision == 1, "Published snapshot mutated");
	state.gaze.available = false;
	state.body.controls[static_cast<std::size_t>(BodyControl::LeftFoot)].tracked = false;
	state.trackers[0].connected = false;
	state.controllers[0].connected = false;
	auto disconnected = tracking.Publish({Duration{7000000000000000200LL}}, state);
	Require(disconnected.gaze.flags == 0 && disconnected.trackers[0].pose.flags == 0 &&
				disconnected.controllers[0].pose.flags == 0,
			"Disconnection activity");
	Require(disconnected.trackers[0].device.identity == first.trackers[0].device.identity,
			"Disconnection changed identity");
	Require(disconnected.body.joints[static_cast<std::size_t>(BodyJoint::LeftFootAnkle)].flags == ValidPoseFlags,
			"Body loss is inferred");
	Require(tracking.Sample({Duration{7000000000000000150LL}})->gaze.flags == second.gaze.flags,
			"Future loss changed earlier state");
	state.body.active = false;
	state.trackers[0].connected = true;
	auto bodyLost = TrackingSystem::Evaluate({Duration{7000000000000000250LL}}, state, BodyModel{});
	Require(!bodyLost.body.active && bodyLost.trackers[0].pose.flags == TrackedPoseFlags,
			"Body sensor loss disconnected independent tracker");
	state.focused = false;
	auto unfocused = tracking.Publish({Duration{7000000000000000300LL}}, state);
	Require(!unfocused.body.active && unfocused.gaze.flags == 0 && unfocused.controllers[1].pose.flags == 0,
			"Focus loss");
	Require(!tracking.Sample(first.time), "History budget not enforced");
	Require(tracking.Sample({Duration{7000000000000000400LL}})->time == unfocused.time,
			"Future query fabricated sensor time");
	bool rejected = false;
	try
	{
		tracking.Publish(second.time, state);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Out-of-order sample accepted");
	rejected = false;
	try
	{
		tracking.Publish(unfocused.time, state);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Duplicate time overwrote immutable sample");
	tracking.Reset();
	state = {};
	tracking.Publish({Duration{10}}, state);
	auto proportions = BodyProportions{};
	proportions.hipWidth = 0.3f;
	tracking.SetBodyProportions(proportions);
	auto changed = tracking.Publish({Duration{20}}, state);
	Require(changed.body.skeletonRevision == 2 && tracking.Sample({Duration{15}})->body.skeletonRevision == 1,
			"Skeleton configuration leaked backward");
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

	PoseHistory inventories;
	auto inventory = TrackingSystem::Evaluate({Duration{10}}, TrackingState{}, BodyModel{});
	inventory.trackers.resize(MaxTrackerDevices);
	inventories.Push(inventory);
	inventory.time = {Duration{20}};
	inventory.trackers.back().pose.pose.position.x = 9;
	inventories.Push(inventory);
	Require(inventories.Sample({Duration{15}})->trackers.size() == MaxTrackerDevices &&
				inventories.Sample({Duration{15}})->trackers.back().pose.pose.position.x == 0,
			"Historical reconstruction silently truncated a larger published inventory");
	inventory.time = {Duration{30}};
	inventory.trackers.clear();
	inventories.Push(inventory);
	Require(inventories.Sample({Duration{25}})->trackers.size() == MaxTrackerDevices &&
				inventories.Sample({Duration{30}})->trackers.empty(),
			"Tracker removal rewrote an earlier inventory");
	inventory.time = {Duration{40}};
	inventories.Push(inventory);
	Require(inventories.Sample({Duration{35}})->trackers.empty(), "Empty inventory reconstructed phantom trackers");
	inventory.time = {Duration{50}};
	inventory.trackers.resize(MaxTrackerDevices + 1);
	bool rejectedInventory = false;
	try
	{
		inventories.Push(inventory);
	}
	catch (const std::invalid_argument&)
	{
		rejectedInventory = true;
	}
	Require(rejectedInventory && inventories.Size() == 4, "Over-capacity inventory changed pose history");
	std::atomic_bool failed = false;
	std::thread reader(
		[&]
		{
			for (int count = 0; count < 100; ++count)
				if (!tracking.Sample({Duration{1000}}))
					failed = true;
		});
	for (int count = 21; count < 121; ++count)
		tracking.Publish({Duration{count}}, state);
	reader.join();
	Require(!failed, "Concurrent historical read");
	std::cout << "Tracking tests passed\n";
}
