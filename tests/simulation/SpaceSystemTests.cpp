#include "../TestSupport.h"
#include "Math.h"
#include "simulation/SpaceSystem.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>

namespace
{
bool Near(float a, float b)
{
	return std::abs(a - b) < 0.0001f;
}

void GenerationHistoryAndBounds()
{
	using namespace simxr;
	TrackingSystem tracking;
	EventQueue events;
	const auto session = reinterpret_cast<XrSession>(600);
	const auto action = reinterpret_cast<XrAction>(601);
	SpaceSystem spaces(
		session, tracking,
		[](XrAction, XrPath, RuntimeTime time)
		{
			return PoseSample{IdentityPose(), TrackedPoseFlags, time};
		},
		[action](XrAction candidate, XrPath)
		{
			return candidate == action ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
		},
		&events);
	XrSpace local = XR_NULL_HANDLE;
	XrSpace stage = XR_NULL_HANDLE;
	XrSpace view = XR_NULL_HANDLE;
	XrSpace actionSpace = XR_NULL_HANDLE;
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, IdentityPose(), local) == XR_SUCCESS &&
				spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, IdentityPose(), stage) == XR_SUCCESS &&
				spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, IdentityPose(), view) == XR_SUCCESS &&
				spaces.CreateActionSpace(action, XR_NULL_PATH, IdentityPose(), actionSpace) == XR_SUCCESS,
			"Prepare spaces retained across generations");

	ReferenceSpaceState oldState;
	oldState.local.position.x = 40;
	oldState.stage.position.z = -30;
	oldState.stageBounds = XrExtent2Df{8, 9};
	Require(spaces.Restore({Duration{5000}}, oldState) == XR_SUCCESS, "Prepare old future origin");
	const auto previousEvents = events.Observe();
	Require(previousEvents.size() == 2, "Prepare reference-space notifications");

	ReferenceSpaceState nextState;
	nextState.local = {{0, 0, 0, 1.005f}, {3, 1.7f, -2}};
	nextState.stage = {EulerAngles(0.5f, 0, 0), {-4, 0, 6}};
	nextState.stageBounds = std::nullopt;
	const RuntimeTime epoch{Duration{10}};
	Require(spaces.StartGeneration(epoch, nextState) == XR_SUCCESS, "Start an earlier reference-space epoch");
	Require(spaces.Owns(local) && spaces.Owns(stage) && spaces.Owns(view) && spaces.Owns(actionSpace),
			"Starting a generation invalidated created space handles");
	const auto captured = spaces.RecordingState(epoch);
	Require(Near(captured.local.orientation.w, 1) && Near(captured.local.position.x, 3) &&
				Near(captured.stage.position.z, 6) && !captured.stageBounds,
			"Generation state did not normalize poses and replace bounds");
	Require(spaces.CanPublish(epoch) && !spaces.CanPublish({Duration{9}}),
			"Generation reset retained the old publication time floor");
	const auto afterStart = events.Observe();
	Require(afterStart.size() == previousEvents.size() && afterStart.back().sequence == previousEvents.back().sequence,
			"Starting a generation emitted reference-change events");

	PoseSample pose;
	Require(spaces.WorldPose(local, {Duration{9}}, pose) == XR_SUCCESS && pose.flags == 0,
			"Prior-generation origin remained visible before the new epoch");
	Require(spaces.WorldPose(local, {Duration{5000}}, pose) == XR_SUCCESS && pose.flags == TrackedPoseFlags &&
				Near(pose.pose.position.x, 3),
			"Prior-generation future origin survived reset");
	SpaceLocation relative;
	Require(spaces.Locate(local, stage, epoch, relative) == XR_SUCCESS, "Locate retained generation handles");
	const auto expected = RelativePose(captured.local, captured.stage);
	Require(Near(relative.pose.pose.position.x, expected.position.x) &&
				Near(relative.pose.pose.position.z, expected.position.z),
			"Generation reset changed space-offset semantics");
	XrExtent2Df bounds{99, 99};
	Require(spaces.Bounds(XR_REFERENCE_SPACE_TYPE_STAGE, bounds) == XR_SPACE_BOUNDS_UNAVAILABLE && bounds.width == 0 &&
				bounds.height == 0,
			"Unavailable generation bounds retained previous dimensions");

	ReferenceSpaceState invalid = captured;
	invalid.local.position.x = std::numeric_limits<float>::quiet_NaN();
	Require(spaces.StartGeneration({Duration{20}}, invalid) == XR_ERROR_POSE_INVALID,
			"Generation accepted a nonfinite pose");
	invalid = captured;
	invalid.stage.orientation = {0, 0, 0, 0};
	Require(spaces.StartGeneration({Duration{20}}, invalid) == XR_ERROR_POSE_INVALID,
			"Generation accepted a degenerate quaternion");
	invalid = captured;
	invalid.stageBounds = XrExtent2Df{-1, 2};
	Require(spaces.StartGeneration({Duration{20}}, invalid) == XR_ERROR_POSE_INVALID,
			"Generation accepted invalid bounds");
	Require(spaces.StartGeneration({}, nextState) == XR_ERROR_TIME_INVALID &&
				spaces.StartGeneration({Duration{-1}}, nextState) == XR_ERROR_TIME_INVALID,
			"Generation accepted a nonpositive epoch");
	Require(SameReferenceSpaces(spaces.RecordingState(epoch), captured) && spaces.CanPublish(epoch) &&
				events.Observe().back().sequence == previousEvents.back().sequence,
			"Rejected generation partially changed history, bounds or events");

	nextState = captured;
	nextState.stageBounds = XrExtent2Df{3, 4};
	Require(spaces.StartGeneration({Duration{20}}, nextState) == XR_SUCCESS &&
				spaces.Bounds(XR_REFERENCE_SPACE_TYPE_STAGE, bounds) == XR_SUCCESS && bounds.width == 3 &&
				bounds.height == 4,
			"New generation did not restore available bounds");
	Require(spaces.WorldPose(local, epoch, pose) == XR_SUCCESS && pose.flags == 0,
			"Second generation retained first-generation history");
	TrackingState tracked;
	tracking.Publish({Duration{20}}, tracked);
	Require(spaces.Recenter({Duration{20}}) == XR_ERROR_TIME_INVALID && spaces.Recenter({Duration{21}}) == XR_SUCCESS &&
				spaces.Recenter({Duration{21}}) == XR_ERROR_TIME_INVALID,
			"Generation reset changed direct recenter duplicate-time behavior");
	Require(spaces.Bounds(XR_REFERENCE_SPACE_TYPE_STAGE, bounds) == XR_SUCCESS && bounds.width == 3,
			"Direct recenter replaced the generation's stage bounds");
}

} // namespace

namespace
{
void FiniteLocations()
{
	using namespace simxr;
	TrackingSystem tracking;
	TrackingState state;
	tracking.Publish({Duration{1000000}}, state);
	state.head.pose.position.x = 2e38f;
	tracking.Publish({Duration{2000000}}, state);
	EventQueue events;
	SpaceSystem spaces(reinterpret_cast<XrSession>(801), tracking, {}, {}, &events);
	XrSpace view{}, stage{}, distant{}, nearLimit{};
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, IdentityPose(), view) == XR_SUCCESS &&
				spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, IdentityPose(), stage) == XR_SUCCESS &&
				spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, {{0, 0, 0, 1}, {-2e38f, 0, 0}}, distant) ==
					XR_SUCCESS &&
				spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, {{0, 0, 0, 1}, {2e38f, 0, 0}}, nearLimit) ==
					XR_SUCCESS,
			"Create finite-limit reference spaces");
	SpaceLocation result;
	Require(spaces.Locate(view, stage, {Duration{2000000}}, result) == XR_SUCCESS &&
				!(result.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) &&
				(result.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT),
			"Unrepresentable velocity marked valid");
	constexpr auto positionBits = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	Require(spaces.Locate(view, distant, {Duration{2000000}}, result) == XR_SUCCESS &&
				!(result.pose.flags & positionBits) && (result.pose.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT),
			"Unrepresentable relative position marked valid or lost valid orientation");
	PoseSample world;
	Require(spaces.WorldPose(nearLimit, {Duration{2000000}}, world) == XR_SUCCESS && !(world.flags & positionBits),
			"Unrepresentable world offset marked valid");
	XrSpace positive{};
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, {{0, 0, 0, 1}, {2e38f, 0, 0}}, positive) ==
					XR_SUCCESS &&
				spaces.Locate(positive, distant, {Duration{2000000}}, result) == XR_SUCCESS &&
				!(result.pose.flags & positionBits),
			"Same-entity relative overflow marked valid");
	ReferenceSpaceState origin;
	origin.local.position.x = -2e38f;
	Require(spaces.Restore({Duration{3000000}}, origin) == XR_SUCCESS, "Set distant origin");
	origin.local.position.x = 2e38f;
	Require(spaces.Restore({Duration{4000000}}, origin) == XR_SUCCESS, "Move distant origin");
	const auto history = events.Observe();
	Require(!std::get<ReferenceSpaceEvent>(history[history.size() - 2].event).poseValid,
			"Unrepresentable reference change marked valid");
	state.body.joints[static_cast<std::size_t>(BodyJoint::Head)] =
		BodyJointOverride{XrPosef{{0, 0, 0, 1}, {2e38f, 1.7f, 0}}, true};
	bool rejected = false;
	try
	{
		tracking.Publish({Duration{3000000}}, state);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && tracking.Sample({Duration{3000000}})->time == RuntimeTime{Duration{2000000}},
			"Rejected generated body partially published tracking history");
}
} // namespace

int main()
{
	FiniteLocations();
	GenerationHistoryAndBounds();
	using namespace simxr;
	auto owner = reinterpret_cast<XrSession>(100);
	auto action = reinterpret_cast<XrAction>(101);
	TrackingSystem tracking;
	TrackingState state;
	RuntimeTime first{Duration{1000000000}}, last{Duration{2000000000}};
	state.head.pose = {EulerAngles(0, 0, 0), {1, 1.7f, 2}};
	tracking.Publish(first, state);
	state.head.pose = {EulerAngles(1, 0, 0), {3, 1.7f, 2}};
	tracking.Publish(last, state);
	EventQueue events;
	SpaceSystem spaces(
		owner, tracking,
		[&](XrAction, XrPath, RuntimeTime time)
		{
			auto snapshot = tracking.Sample(time);
			return snapshot ? snapshot->controllers[0].pose : PoseSample{};
		},
		[&](XrAction value, XrPath path)
		{
			return value != action ? XR_ERROR_HANDLE_INVALID : path == 2 ? XR_SUCCESS : XR_ERROR_PATH_UNSUPPORTED;
		},
		&events);
	XrSpace local{}, stage{}, view{}, shifted{}, controller{}, controllerOffset{};
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, IdentityPose(), local) == XR_SUCCESS,
			"Create local");
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, IdentityPose(), stage) == XR_SUCCESS,
			"Create stage");
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, IdentityPose(), view) == XR_SUCCESS,
			"Create view");
	auto offset = IdentityPose();
	offset.position = {0, 0, -1};
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, offset, shifted) == XR_SUCCESS,
			"Create view offset");
	Require(spaces.CreateActionSpace(action, 2, IdentityPose(), controller) == XR_SUCCESS, "Create action");
	Require(spaces.CreateActionSpace(action, 2, offset, controllerOffset) == XR_SUCCESS, "Create action offset");
	SpaceSample sampled;
	Require(spaces.Sample(controllerOffset, first, sampled) == XR_SUCCESS &&
		sampled.referenceType == XR_REFERENCE_SPACE_TYPE_MAX_ENUM && sampled.poseOffset.position.z == -1,
		"Action-space snapshot fabricated a reference-space attachment");
	XrSpace unchanged = shifted;
	Require(spaces.CreateActionSpace(reinterpret_cast<XrAction>(999), 2, IdentityPose(), unchanged) ==
					XR_ERROR_HANDLE_INVALID &&
				unchanged == shifted,
			"Foreign action accepted");
	Require(spaces.CreateActionSpace(action, 3, IdentityPose(), unchanged) == XR_ERROR_PATH_UNSUPPORTED,
			"Foreign subaction accepted");
	SpaceSystem other(reinterpret_cast<XrSession>(102), tracking);
	SpaceLocation result;
	Require(!other.Owns(local) && other.Locate(local, stage, first, result) == XR_ERROR_HANDLE_INVALID,
			"Cross-session spaces");
	Require(spaces.Locate(view, local, first, result) == XR_SUCCESS && Near(result.pose.pose.position.x, 1),
			"View world position");
	auto baseOffset = XrPosef{EulerAngles(1.5707963268f, 0, 0), {1, 0, 0}};
	XrSpace rotatedBase{};
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, baseOffset, rotatedBase) == XR_SUCCESS,
			"Create rotated base");
	Require(spaces.Locate(view, rotatedBase, first, result) == XR_SUCCESS, "Locate rotated base");
	auto relative = RelativePose(tracking.Sample(first)->head.pose, baseOffset);
	Require(Near(result.pose.pose.position.x, relative.position.x) &&
				Near(result.pose.pose.position.z, relative.position.z),
			"Rotated translated base transform");
	RuntimeTime middle{Duration{1500000000}};
	Require(spaces.Locate(view, local, middle, result) == XR_SUCCESS && Near(result.pose.pose.position.x, 2),
			"Historical view interpolation");
	Require((result.velocityFlags & XR_SPACE_VELOCITY_LINEAR_VALID_BIT) &&
				std::abs(result.linearVelocity.x - 2) < 0.001f,
			"Historical linear velocity");
	Require((result.velocityFlags & XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) &&
				std::abs(result.angularVelocity.y - 1) < 0.001f,
			"Historical angular velocity");
	Require(spaces.Locate(shifted, view, middle, result) == XR_SUCCESS && Near(result.pose.pose.position.z, -1) &&
				result.pose.flags == TrackedPoseFlags,
			"Common view entity offset");
	Require(result.velocityFlags == (XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT) &&
				result.linearVelocity.x == 0,
			"Common entity velocity");
	Require(spaces.Locate(view, local, {Duration{3000000000}}, result) == XR_SUCCESS && result.velocityFlags == 0 &&
				result.pose.sampleTime == last,
			"Future sample fabricates velocity");
	Require(spaces.Locate(view, local, {Duration{1}}, result) == XR_SUCCESS && result.pose.flags == 0,
			"Unavailable old sample");
	Require(spaces.Locate(view, local, {}, result) == XR_ERROR_TIME_INVALID, "Zero time");
	XrExtent2Df bounds;
	Require(spaces.Bounds(XR_REFERENCE_SPACE_TYPE_STAGE, bounds) == XR_SUCCESS && bounds.width == 2, "Stage bounds");
	Require(spaces.SetStageBounds(std::nullopt) == XR_SUCCESS &&
				spaces.Bounds(XR_REFERENCE_SPACE_TYPE_STAGE, bounds) == XR_SPACE_BOUNDS_UNAVAILABLE &&
				bounds.width == 0,
			"Unavailable bounds");
	Require(spaces.Bounds(XR_REFERENCE_SPACE_TYPE_VIEW, bounds) == XR_SPACE_BOUNDS_UNAVAILABLE, "View bounds");
	Require(spaces.SetStageBounds(XrExtent2Df{-1, 2}) == XR_ERROR_VALIDATION_FAILURE, "Invalid stage bounds");
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_MAX_ENUM, IdentityPose(), unchanged) ==
				XR_ERROR_REFERENCE_SPACE_UNSUPPORTED,
			"Unknown reference space");
	auto bad = IdentityPose();
	bad.orientation.w = 1.02f;
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, bad, unchanged) == XR_ERROR_POSE_INVALID,
			"Invalid pose");
	Require(spaces.Recenter(middle) == XR_SUCCESS, "Recenter");
	Require(spaces.Recenter(middle) == XR_ERROR_TIME_INVALID, "Duplicate recenter timestamp");
	Require(spaces.Locate(view, local, {Duration{1499999999}}, result) == XR_SUCCESS &&
				Near(result.pose.pose.position.x, 2),
			"Recenter applied before boundary");
	Require(spaces.Locate(view, local, middle, result) == XR_SUCCESS && Near(result.pose.pose.position.x, 0) &&
				Near(result.pose.pose.position.y, 0),
			"Recenter boundary local origin");
	Require(result.velocityFlags == 0, "Recenter impulse velocity");
	Require(spaces.Locate(view, stage, middle, result) == XR_SUCCESS && Near(result.pose.pose.position.y, 1.7f),
			"Stage recenter floor");
	auto observed = events.Observe();
	Require(observed.size() == 2, "Recenter events");
	auto event = std::get<ReferenceSpaceEvent>(observed[0].event);
	Require(event.referenceSpace == XR_REFERENCE_SPACE_TYPE_LOCAL && event.changeTime == middle &&
				Near(event.poseInPreviousSpace.position.x, 2) && Near(event.poseInPreviousSpace.position.y, 1.7f),
			"Natural origin event delta");
	PoseSample world;
	Require(spaces.WorldPose(shifted, middle, world) == XR_SUCCESS, "World pose");
	auto expected = Compose(tracking.Sample(middle)->head.pose, offset);
	Require(Near(world.pose.position.x, expected.position.x) && Near(world.pose.position.z, expected.position.z),
			"Rotated world offset");
	state.focused = false;
	tracking.Publish({Duration{3000000000}}, state);
	Require(spaces.Locate(controllerOffset, controller, {Duration{3000000000}}, result) == XR_SUCCESS &&
				result.pose.flags == TrackedPoseFlags && Near(result.pose.pose.position.z, -1),
			"Common untracked entity relation");
	Require(spaces.Locate(controller, stage, {Duration{3000000000}}, result) == XR_SUCCESS && result.pose.flags == 0,
			"Unavailable action tracking");
	std::atomic<XrSpace> changing{XR_NULL_HANDLE};
	std::atomic<bool> done = false;
	std::thread observer(
		[&]
		{
			while (!done.load())
			{
				auto handle = changing.load();
				PoseSample pose;
				auto status = spaces.WorldPose(handle, last, pose);
				Require(status == XR_SUCCESS || status == XR_ERROR_HANDLE_INVALID, "Concurrent space retirement");
			}
		});
	for (int index = 0; index < 100; ++index)
	{
		XrSpace handle{};
		spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, IdentityPose(), handle);
		changing.store(handle);
		spaces.Destroy(handle);
	}
	done.store(true);
	observer.join();
	spaces.RemoveAction(action);
	Require(!spaces.Owns(controller) && !spaces.Owns(controllerOffset) && spaces.Owns(view),
			"Action-dependent cleanup");
	Require(spaces.Destroy(view) == XR_SUCCESS && spaces.Destroy(view) == XR_ERROR_HANDLE_INVALID,
			"Stale space handle");
	spaces.Clear();
	Require(!spaces.Owns(local) && !spaces.Owns(stage), "Session space cleanup");
	std::cout << "Space system tests passed\n";
}
