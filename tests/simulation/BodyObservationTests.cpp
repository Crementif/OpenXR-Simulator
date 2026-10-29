#include "../TestSupport.h"
#include "Pch.h"
#include "math/Math.h"
#include "runtime/extensions/BodyTracking.h"
#include "simulation/BodyObservation.h"
#include "simulation/CommandScheduler.h"

#include <iostream>

namespace simxr::tests
{
static BodyObservation Observed(BodyJointSet set)
{
	BodyObservation observation;
	observation.jointSet = set;
	observation.located = true;
	observation.requestedTime = {Duration{90}};
	const bool fb = set == BodyJointSet::UpperBodyFB || set == BodyJointSet::FullBodyMETA;
	if (fb)
	{
		observation.returnedTime = RuntimeTime{Duration{80}};
		observation.active = true;
		observation.confidence = .75f;
		observation.skeletonRevision = 0;
		observation.skeleton.emplace();
	}
	else
		observation.allJointPosesTracked = false;
	for (const auto joint : BodyModel::JointMap(set))
	{
		const auto index = static_cast<std::size_t>(joint);
		observation.joints[index] = ObservedBodyJoint{IdentityPose(), ValidPoseFlags};
		observation.joints[index]->pose.position.x = 10;
		if (fb)
		{
			(*observation.skeleton)[index] = SkeletonJoint{joint, {}, IdentityPose()};
			(*observation.skeleton)[index]->bindPose.position.x = 3;
		}
	}
	return observation;
}

static void ReplayTime()
{
	constexpr std::int64_t source = 9007199254740993LL;
	for (const auto set :
		 {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA, BodyJointSet::WithoutArmsBD, BodyJointSet::FullBodyBD})
	{
		auto observation = Observed(set);
		observation.requestedTime = {Duration{source - 20}};
		if (observation.returnedTime)
			observation.returnedTime = RuntimeTime{Duration{source - 30}};
		CommandState state;
		CommandConfiguration baseline;
		CommandTransaction transaction;
		transaction.source = {CommandSourceKind::Rig, 7};
		transaction.commands = {TakeControl{ControlChannel::Body},
								SetBodyControls{{}, observation, RuntimeTime{Duration{source}}}};
		Require(state.Apply(transaction, baseline, {Duration{1000}}) == CommandResult::Applied,
				"Replay body applies at actual runtime boundary");
		const auto& applied = std::get<SetBodyControls>(transaction.commands[1]);
		Require(!applied.observationOrigin && applied.observation->requestedTime.value.count() == 980 &&
					(!applied.observation->returnedTime || applied.observation->returnedTime->value.count() == 970),
				"Acknowledgment loses exact replay age or retains a second rebasing instruction");
		Require(state.Apply(transaction, baseline, {Duration{2000}}) == CommandResult::Applied &&
					state.Merge(baseline).tracking.bodyObservation->requestedTime.value.count() == 980,
				"Resolved body timestamps rebased twice");
		auto invalid = transaction;
		invalid.commands = {SetBodyControls{{}, observation, RuntimeTime{Duration{source}}}};
		Require(state.Apply(invalid, baseline, {Duration{10}}) == CommandResult::Invalid &&
					state.Merge(baseline).tracking.bodyObservation->requestedTime.value.count() == 980 &&
					std::get<SetBodyControls>(invalid.commands[0]).observationOrigin.has_value(),
				"Rebase underflow changed state or consumed the rejected command");
		observation.requestedTime = {Duration{source + 20}};
		invalid.commands = {SetBodyControls{{}, observation, RuntimeTime{Duration{source}}}};
		Require(state.Apply(invalid, baseline, {Duration{std::numeric_limits<std::int64_t>::max() - 10}}) ==
					CommandResult::Invalid,
				"Rebase overflow was accepted");
		invalid.commands = {SetBodyControls{{}, {}, RuntimeTime{Duration{source}}}};
		Require(state.Apply(invalid, baseline, {Duration{1000}}) == CommandResult::Invalid,
				"Replay anchor without body data was accepted");
		if (observation.active)
		{
			observation.active = false;
			observation.returnedTime = RuntimeTime{};
			for (auto& joint : observation.joints)
				if (joint)
					joint->flags = 0;
			const auto inactive = RebaseBodyObservation(observation, {Duration{source}}, {Duration{1000}});
			Require(inactive.returnedTime && inactive.returnedTime->value.count() == 0 &&
						inactive.requestedTime.value.count() == 1020,
					"Inactive provider zero sentinel was converted into a timestamp");
		}
	}
}

static void Ownership()
{
	CommandState state;
	CommandConfiguration baseline;
	const CommandSource device{CommandSourceKind::Device, 1};
	CommandTransaction claim;
	claim.source = device;
	claim.commands = {TakeControl{ControlChannel::Body}, SetBodyControls{{}, Observed(BodyJointSet::UpperBodyFB)}};
	Require(state.Apply(claim, baseline, {Duration{100}}) == CommandResult::Applied &&
				state.Merge(baseline).tracking.bodyObservation &&
				std::get<SetBodyControls>(claim.commands[1]).observation,
			"Owned body commands publish and acknowledge the observation");
	auto foreign = claim;
	foreign.source = {CommandSourceKind::Manual, 2};
	Require(state.Apply(foreign, baseline, {Duration{110}}) == CommandResult::Conflict,
			"Physical body ownership cannot be silently taken by manual input");
	auto invalid = claim;
	std::get<SetBodyControls>(invalid.commands[1]).observation->confidence = 2;
	Require(state.Apply(invalid, baseline, {Duration{120}}) == CommandResult::Invalid &&
				state.Merge(baseline).tracking.bodyObservation->confidence == .75f,
			"Rejected body transaction preserves the previous observation atomically");
	CommandTransaction scenario;
	scenario.source = device;
	scenario.commands = {SetScenario{FailureScenario::BodyUnavailable, true}};
	Require(state.Apply(scenario, baseline, {Duration{130}}) == CommandResult::Applied,
			"Observed body supports existing availability scenarios");
	const auto unavailable = state.Merge(baseline);
	for (const auto& joint : unavailable.tracking.bodyObservation->joints)
		Require(!joint || !joint->flags, "Body unavailable scenario masks observed validity");
	Require(state.RecordingOrigin(baseline, {Duration{130}}).configuration.tracking.bodyObservation->active == true,
			"Scenario masking must not erase recorded source state");
	scenario.commands = {SetScenario{FailureScenario::BodyUnavailable, false}, SetBodyControls{BodyControls{}}};
	Require(state.Apply(scenario, baseline, {Duration{140}}) == CommandResult::Applied &&
				!state.Merge(baseline).tracking.bodyObservation,
			"Authored body edits clear observed-body precedence");
	Require(state.Apply(claim, baseline, {Duration{150}}) == CommandResult::Applied, "Restore body observation");
	state.ReleaseSource(device);
	Require(!state.Merge(baseline).tracking.bodyObservation, "Release restores the authored baseline");
	CommandScheduler scheduler;
	claim.id = 1;
	claim.target.frame = FrameId{1};
	Require(scheduler.Submit(claim).result == CommandResult::Queued, "Queue body observation");
	Require(scheduler.Submit(claim).result == CommandResult::Queued, "Identical body retry");
	for (unsigned field = 0; field < 5; ++field)
	{
		auto changed = claim;
		auto& value = *std::get<SetBodyControls>(changed.commands[1]).observation;
		if (field == 0)
			value.returnedTime = RuntimeTime{Duration{81}};
		if (field == 1)
			value.confidence = .5f;
		if (field == 2)
			value.joints[static_cast<std::size_t>(BodyJoint::Head)]->flags = TrackedPoseFlags;
		if (field == 3)
			(*value.skeleton)[static_cast<std::size_t>(BodyJoint::Head)]->bindPose.position.x = 4;
		if (field == 4)
			std::get<SetBodyControls>(changed.commands[1]).observationOrigin = RuntimeTime{Duration{100}};
		Require(scheduler.Submit(changed).result == CommandResult::Conflict,
				"Body retry identity must include metadata, validity and skeleton values");
	}
}

static void Publication()
{
	TrackingSystem tracking;
	TrackingState state;
	state.bodyObservation = Observed(BodyJointSet::UpperBodyFB);
	state.body.preset = BodyPreset::Seated;
	const auto first = tracking.Publish({Duration{100}}, state);
	const auto head = static_cast<std::size_t>(BodyJoint::Head);
	const auto foot = static_cast<std::size_t>(BodyJoint::LeftFootAnkle);
	Require(first.body.joints[head].pose.position.x == 10 && first.body.joints[head].flags == ValidPoseFlags &&
				first.body.sampleTime.value.count() == 80 && first.body.active,
			"Observed body bypasses generated pose/flags and retains returned sample time");
	Require(first.body.joints[foot].flags == 0 && first.trackers[2].pose.flags == 0,
			"Missing lower body cannot create a tracked joint or attached tracker");
	state.bodyObservation->joints[head]->pose.position.x = 20;
	state.bodyObservation->returnedTime = RuntimeTime{Duration{180}};
	state.bodyObservation->requestedTime = {Duration{190}};
	tracking.Publish({Duration{200}}, state);
	const auto middle = tracking.Sample({Duration{150}});
	Require(middle && middle->body.joints[head].pose.position.x == 10 && middle->body.sampleTime.value.count() == 80,
			"History must not interpolate observations or overwrite their sample timestamps");
	state.focused = false;
	const auto lost = TrackingSystem::Evaluate({Duration{210}}, state, BodyModel{});
	Require(!lost.body.active && lost.body.joints[head].flags == 0 &&
				lost.input.bodyObservation->joints[head]->flags == ValidPoseFlags,
			"Focus loss invalidates evaluated joints while retaining original provider provenance");
	state.focused = true;
	state.bodyObservation->joints[head]->flags = XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	bool rejected = false;
	try
	{
		tracking.Publish({Duration{220}}, state);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && tracking.Sample({Duration{220}})->time.value.count() == 200,
			"Malformed observations cannot partially publish");
	state.bodyObservation.reset();
	const auto generated = tracking.Publish({Duration{300}}, state);
	Require(generated.body.joints[foot].flags != 0 && !generated.input.bodyObservation,
			"Releasing observed body restores authored rig evaluation");
}

static void RuntimeQueries()
{
	const std::array extensions{XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_BD_BODY_TRACKING_EXTENSION_NAME,
								XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME};
	TrackingSystem tracking;
	TrackingState state;
	state.bodyObservation = Observed(BodyJointSet::UpperBodyFB);
	tracking.Publish({Duration{100}}, state);
	const auto session = reinterpret_cast<XrSession>(901);
	SpaceSystem spaces(session, tracking);
	XrSpace base{};
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, IdentityPose(), base) == XR_SUCCESS,
			"Body base");
	BodyTracking bodies(session, tracking, spaces, EnabledExtensions(extensions));
	XrBodyTrackerFB handle{};
	XrBodyTrackerCreateInfoFB create{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB, nullptr, XR_BODY_JOINT_SET_DEFAULT_FB};
	Require(bodies.CreateFB(&create, &handle) == XR_SUCCESS, "Body handle");
	XrBodyTrackingCalibrationInfoMETA height{XR_TYPE_BODY_TRACKING_CALIBRATION_INFO_META, nullptr, 2.5f};
	Require(bodies.SuggestCalibrationFB(handle, &height) == XR_SUCCESS, "Authored height override");
	std::array<XrBodyJointLocationFB, XR_BODY_JOINT_COUNT_FB> joints{};
	XrBodyJointLocationsFB locations{XR_TYPE_BODY_JOINT_LOCATIONS_FB};
	locations.jointCount = joints.size();
	locations.jointLocations = joints.data();
	XrBodyJointsLocateInfoFB locate{XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB, nullptr, base, 100};
	Require(bodies.LocateFB(handle, &locate, &locations) == XR_SUCCESS && locations.isActive && locations.time == 80 &&
				locations.confidence == .75f && joints[XR_BODY_JOINT_HEAD_FB].pose.position.x == 10,
			"Runtime query preserves observed pose/time rather than regenerating calibrated body");
	const auto revision = locations.skeletonChangedCount;
	std::array<XrBodySkeletonJointFB, XR_BODY_JOINT_COUNT_FB> skeletonJoints{};
	XrBodySkeletonFB skeleton{XR_TYPE_BODY_SKELETON_FB, nullptr, static_cast<std::uint32_t>(skeletonJoints.size()),
							  skeletonJoints.data()};
	Require(bodies.SkeletonFB(handle, &skeleton) == XR_SUCCESS &&
				skeletonJoints[XR_BODY_JOINT_HEAD_FB].pose.position.x == 3,
			"Runtime returns provider rest skeleton without generated proportions or world transform");
	++*state.bodyObservation->skeletonRevision;
	tracking.Publish({Duration{200}}, state);
	locate.time = 200;
	Require(bodies.LocateFB(handle, &locate, &locations) == XR_SUCCESS &&
				locations.skeletonChangedCount == revision + 1,
			"Provider skeleton revision notifies existing game tracker");
	(*state.bodyObservation->skeleton)[static_cast<std::size_t>(BodyJoint::Head)]->bindPose.position.x = 4;
	tracking.Publish({Duration{250}}, state);
	locate.time = 250;
	Require(bodies.LocateFB(handle, &locate, &locations) == XR_SUCCESS &&
				locations.skeletonChangedCount == revision + 2,
			"Reconnected provider with a reused native revision must still notify changed rest skeleton");
	state.bodyObservation = Observed(BodyJointSet::WithoutArmsBD);
	tracking.Publish({Duration{300}}, state);
	locate.time = 300;
	Require(bodies.LocateFB(handle, &locate, &locations) == XR_SUCCESS && !locations.isActive,
			"Incomplete provider schema must not activate FB with missing joints");
	for (const auto& joint : joints)
		Require(!joint.locationFlags, "Inactive FB locations must be invalid");
	skeletonJoints[0].joint = 99;
	Require(bodies.SkeletonFB(handle, &skeleton) == XR_ERROR_RUNTIME_FAILURE && skeletonJoints[0].joint == 99,
			"Absent provider skeleton cannot be replaced with generated body or partially overwrite output");
	XrBodyTrackerBD handleBD{};
	XrBodyTrackerCreateInfoBD createBD{XR_TYPE_BODY_TRACKER_CREATE_INFO_BD, nullptr,
									   XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD};
	Require(bodies.CreateBD(&createBD, &handleBD) == XR_SUCCESS, "BD handle");
	std::array<XrBodyJointLocationBD, XR_BODY_JOINT_COUNT_BD> jointsBD{};
	XrBodyJointLocationsBD locationsBD{XR_TYPE_BODY_JOINT_LOCATIONS_BD, nullptr, XR_FALSE, XR_BODY_JOINT_COUNT_BD,
									   jointsBD.data()};
	XrBodyJointsLocateInfoBD locateBD{XR_TYPE_BODY_JOINTS_LOCATE_INFO_BD, nullptr, base, 300};
	Require(bodies.LocateBD(handleBD, &locateBD, &locationsBD) == XR_SUCCESS && !locationsBD.allJointPosesTracked &&
				jointsBD[XR_BODY_JOINT_PELVIS_BD].locationFlags == ValidPoseFlags &&
				jointsBD[XR_BODY_JOINT_LEFT_HAND_BD].locationFlags == 0,
			"BD reports available partial joints without inventing missing arms");
}
} // namespace simxr::tests

int main()
{
	try
	{
		simxr::tests::ReplayTime();
		simxr::tests::Ownership();
		simxr::tests::Publication();
		simxr::tests::RuntimeQueries();
		std::cout << "Body observation publication passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
