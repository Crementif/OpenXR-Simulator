#include "../TestSupport.h"
#include "app/services/SimulationEdits.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include "simulation/CommandScheduler.h"
#include <cmath>
#include <numbers>

#include <iostream>
#include <limits>
#include <utility>

namespace
{
using namespace simxr;
namespace wire = simxr::control;
namespace app = simxr::app;

wire::Message Observation(const CommandScheduler& scheduler, std::optional<std::array<std::string, 2>> profiles = {})
{
	wire::Message result;
	result.fields.push_back(wire::Field::Bytes(
		wire::tag::Document, wire::SimulationState(scheduler.Snapshot(), false, {}, {}, std::move(profiles)).Encode()));
	result.fields.push_back(wire::Field::Bytes(
		wire::tag::Pipeline, wire::EncodeFields(std::array{wire::Field::U64(wire::tag::NextAdmission, 900)})));
	return result;
}

std::shared_ptr<const AppliedCommandSnapshot> Apply(CommandScheduler& scheduler, std::uint64_t frame)
{
	return scheduler.Apply(FrameId{frame}, SimulationTime{Duration{100}}, RuntimeTime{Duration{200}}, {});
}

template<typename Action>
void Rejected(Action action)
{
	bool rejected = false;
	try
	{
		action();
	}
	catch (const std::exception&)
	{
		rejected = true;
	}
	Require(rejected, "Invalid or unavailable simulation state scheduled a guessed frame");
}

void ObservedFrameScheduling()
{
	CommandScheduler scheduler;
	scheduler.Start();
	Apply(scheduler, 7);
	PoseSample pose;
	pose.pose.orientation.w = 1;
	pose.pose.position.x = 3;
	app::ManualTransaction edit;
	edit.commands = {TakeControl{ControlChannel::Head}, SetHeadPose{pose}};
	auto packet = app::EncodeSimulationEdit(edit, 21, 4, 1, Observation(scheduler));
	auto transaction = wire::DecodeSimulationTransaction(packet.message, 55);
	Require(transaction.target.frame == FrameId{8}, "Editor used a display tick as the command frame selector");
	Require(transaction.source == CommandSource{CommandSourceKind::Manual, 55} && transaction.id == 1,
			"Manual source or transaction identity changed during encoding");
	Require(scheduler.Submit(std::move(transaction)).result == CommandResult::Queued, "Encoded edit was not admitted");
	const auto publication = Apply(scheduler, 8);
	Require(publication->configuration.tracking.head.pose.position.x == 3 && publication->outcomes.size() == 1 &&
				publication->outcomes.front().result == CommandResult::Applied,
			"Edit did not apply at the next command publication");

	edit.frame = FrameId{12};
	edit.latePolicy = CommandLatePolicy::Reject;
	packet = app::EncodeSimulationEdit(edit, 21, 4, 2, {});
	transaction = wire::DecodeSimulationTransaction(packet.message, 55);
	Require(transaction.target.frame == FrameId{12} && transaction.latePolicy == CommandLatePolicy::Reject,
			"Explicit edit target and late policy were overwritten by observation");
}

void DirectPiloting()
{
	CommandScheduler scheduler;
	scheduler.Start();
	Apply(scheduler, 1);
	PoseSample origin;
	origin.flags = TrackedPoseFlags;
	origin.pose.position = {2, 1.7f, -4};
	origin.pose.orientation = EulerAngles(-std::numbers::pi_v<float> / 2, 0, 0);
	app::ManualTransaction edit;
	edit.commands = {TakeControl{ControlChannel::Head},		 SetHeadPose{origin},
					 TakeControl{ControlChannel::LeftPose},	 SetControllerPose{0, {origin, true}},
					 TakeControl{ControlChannel::RightPose}, SetControllerPose{1, {origin, false}}};
	auto transaction =
		wire::DecodeSimulationTransaction(app::EncodeSimulationEdit(edit, 1, 1, 1, Observation(scheduler)).message, 55);
	Require(scheduler.Submit(std::move(transaction)).result == CommandResult::Queued,
			"Initial pilot pose was rejected");
	Apply(scheduler, 2);
	const auto state = Observation(scheduler);
	const auto observed = app::ObservedDevicePose(state, ControlChannel::Head);
	Require(observed.pose.position.x == 2 && observed.pose.position.z == -4, "Piloting jumped to a default pose");
	Require(app::ObservedDevicePose(state, ControlChannel::LeftPose).pose.position.x == 2,
			"Hand piloting read the wrong pose");
	Rejected([&] { app::ObservedDevicePose(state, ControlChannel::RightPose); });
	const auto moved = app::PilotPose(observed, {0, 0, -1}, 0, 0, 0.02f);
	Require(std::abs(moved.pose.position.x - 2.02f) < 0.0001f && std::abs(moved.pose.position.z + 4) < 0.0001f,
			"Forward piloting ignored the device orientation");
	const auto fast = app::PilotPose(observed, {0, 0, -1}, 0, 0, 0.02f, 3);
	Require(std::abs(fast.pose.position.x - 2.06f) < 0.0001f, "Speed modifier depends on frame rate");
	const auto stalled = app::PilotPose(observed, {0, 0, -1}, 0, 0, 10);
	Require(stalled.pose.position.x <= 2.051f, "A UI stall teleported the device");
	const auto rotated = app::PilotPose(observed, {}, 0, 100, 0.02f);
	Require(NormalizedPose(rotated.pose).has_value() && Rotate(rotated.pose.orientation, {0, 0, -1}).y < 1,
			"Pointer motion flipped or invalidated the device pose");
}

void PreserveObservedConfiguration()
{
	CommandConfiguration initial;
	initial.views.ipd = 0.071f;
	initial.tracking.controllers[0].aim = PoseSample{XrPosef{EulerAngles(.2f, .3f, 0), {1, 2, 3}}, ValidPoseFlags};
	initial.tracking.eyeViews.emplace();
	initial.tracking.eyeViews->spaceRevision = 9007199254740993ULL;
	initial.tracking.eyeViews->poses[0] = PoseSample{XrPosef{EulerAngles(.1f, 0, 0), {-.04f, 1.6f, 0}}, ValidPoseFlags};
	initial.tracking.eyeViews->poses[1].flags = 0;
	initial.tracking.eyeViews->fovs[1] = {-.6f, .9f, .7f, -.8f};
	initial.views.outerWidth = 1728;
	initial.views.insetHeight = 640;
	initial.views.outerFov[1].angleRight = 0.61f;
	initial.input.preferredProfiles[1] = "/interaction_profiles/valve/index_controller";
	initial.input.controllers[0].gripForce = 0.63f;
	initial.input.controllers[0].physicalProfile = "/interaction_profiles/htc/vive_controller";
	initial.input.controllers[0].triggerClick = false;
	initial.input.controllers[0].thumbrestTouch = true;
	initial.input.controllers[0].trackpad = {0.3f, -0.2f};
	initial.tracking.body.controls[0].offset = XrVector3f{0.1f, 0.2f, 0.3f};
	initial.tracking.body.joints[3] = BodyJointOverride{XrPosef{{0, 0, 0, 1}, {0.2f, 1.2f, 0}}, false};
	initial.tracking.trackers[2].anchor = BodyJoint::Hips;
	initial.tracking.trackers[2].poseOverride = XrPosef{{0, 0, 0, 1}, {0.1f, 1.4f, -0.5f}};
	CommandScheduler scheduler;
	scheduler.Start(0, initial);
	Apply(scheduler, 1);
	const auto state = Observation(scheduler);
	const auto observed = app::ReadObservedSetup(state);
	Require(observed.values.input.controllers[0].physicalProfile == initial.input.controllers[0].physicalProfile &&
				observed.values.input.controllers[0].triggerClick == false && !observed.controllerProfiles,
			"Observed physical inputs are retained without inventing an effective profile");
	const std::array<std::string, 2> profiles{"/interaction_profiles/khr/simple_controller", ""};
	const auto resolved = app::ReadObservedSetup(Observation(scheduler, profiles));
	Require(resolved.controllerProfiles == profiles &&
				resolved.values.input.controllers[0].physicalProfile != (*resolved.controllerProfiles)[0],
			"Physical and effective profiles remain distinct, including an unbound hand");
	Require(observed.values.tracking.controllers[0].aim &&
				observed.values.tracking.controllers[0].aim->pose.position.z == 3 &&
				observed.values.tracking.controllers[0].aim->flags == ValidPoseFlags &&
				!observed.values.tracking.controllers[1].aim,
			"Companion preserves independent aim without fabricating absent aim");
	const auto& eyes = observed.values.tracking.eyeViews;
	Require(eyes && eyes->spaceRevision == 9007199254740993ULL && eyes->poses[0].pose.position.x == -.04f &&
				eyes->poses[0].flags == ValidPoseFlags && eyes->poses[1].flags == 0 &&
				eyes->fovs[1].angleRight == .9f &&
				eyes->poses[0].pose.orientation.x == initial.tracking.eyeViews->poses[0].pose.orientation.x,
			"Companion preserves physical eye pose, validity, asymmetric FOV and exact space revision");
	Require(observed.bodyOverridesAvailable && observed.trackerAttachmentsAvailable,
			"Hidden configuration is not exposed for safe editing");
	Require(observed.values.input.preferredProfiles == initial.input.preferredProfiles,
			"Controller profiles were replaced by a shared default");
	auto views = observed.values.views;
	views.ipd = 0.067f;
	auto tracker = observed.values.tracking.trackers[2];
	tracker.role = TrackerRole::Camera;
	auto body = observed.values.tracking.body;
	body.preset = BodyPreset::Seated;
	auto controls = observed.values.input.controllers[0];
	controls.primary = true;
	app::ManualTransaction edit;
	edit.commands = {TakeControl{ControlChannel::Views},		SetViewSettings{views},
					 TakeControl{ControlChannel::Trackers},		SetTracker{2, tracker},
					 TakeControl{ControlChannel::Body},			SetBodyControls{body},
					 TakeControl{ControlChannel::LeftControls}, SetControllerControls{0, controls}};
	const auto packet = app::EncodeSimulationEdit(edit, 1, 1, 1, state);
	scheduler.Submit(wire::DecodeSimulationTransaction(packet.message, 55));
	const auto applied = Apply(scheduler, 2);
	Require(applied->outcomes.back().result == CommandResult::Applied, "Observed configuration edit did not apply");
	const auto& result = applied->configuration;
	Require(result.views.ipd == 0.067f && result.views.outerWidth == 1728 && result.views.insetHeight == 640 &&
				result.views.outerFov[1].angleRight == 0.61f,
			"IPD edit reset hidden view settings");
	Require(result.tracking.trackers[2].role == TrackerRole::Camera &&
				result.tracking.trackers[2].anchor == BodyJoint::Hips &&
				result.tracking.trackers[2].poseOverride->position.z == -0.5f,
			"Tracker role edit reset its attachment or pose");
	Require(result.tracking.body.preset == BodyPreset::Seated && result.tracking.body.controls[0].offset->y == 0.2f &&
				result.tracking.body.joints[3]->pose->position.y == 1.2f && !result.tracking.body.joints[3]->tracked,
			"Body preset edit reset joint overrides");
	Require(result.input.controllers[0].primary && result.input.controllers[0].thumbrestTouch &&
				result.input.controllers[0].gripForce == 0.63f && result.input.controllers[0].trackpad.x == 0.3f,
			"Button edit reset unexposed inputs");
	Require(result.input.preferredProfiles == initial.input.preferredProfiles,
			"An unrelated edit changed controller profiles");

	for (const bool enabled : {true, false})
	{
		edit.commands = {TakeControl{app::ScenarioInputChannel(FailureScenario::HeadUntracked)},
						 SetScenario{FailureScenario::HeadUntracked, enabled}};
		scheduler.Submit(wire::DecodeSimulationTransaction(
			app::EncodeSimulationEdit(edit, 1, 1, enabled ? 2 : 3, Observation(scheduler)).message, 55));
		Apply(scheduler, enabled ? 3 : 4);
		const auto after = Observation(scheduler);
		Require(app::ReadObservedSetup(after).scenarios[static_cast<std::size_t>(FailureScenario::HeadUntracked)] ==
					enabled,
				"Scenario state did not follow activation/recovery");
		if (enabled)
			Rejected([&] { app::ObservedDevicePose(after, ControlChannel::Head); });
		else
			Require(app::ObservedDevicePose(after, ControlChannel::Head).flags == TrackedPoseFlags,
					"Recovery did not restore tracking");
	}
	auto hand = observed.values.tracking.controllers[0];
	hand.aim.reset();
	edit.commands = {SetEyeViews{}, TakeControl{ControlChannel::LeftPose}, SetControllerPose{0, hand}};
	scheduler.Submit(wire::DecodeSimulationTransaction(
		app::EncodeSimulationEdit(edit, 1, 1, 4, Observation(scheduler)).message, 55));
	Apply(scheduler, 5);
	const auto cleared = app::ReadObservedSetup(Observation(scheduler));
	Require(!cleared.values.tracking.eyeViews && !cleared.values.tracking.controllers[0].aim,
			"Removing physical view and aim observations cannot retain stale companion state");
}

void SourceKindRange()
{
	for (const auto kind : {std::uint64_t{4}, std::uint64_t{1} << 32, std::numeric_limits<std::uint64_t>::max()})
	{
		wire::Message receipt;
		receipt.fields.push_back(wire::Field::Bytes(
			wire::tag::Document,
			wire::InspectionValue::Object(
				{{"source", wire::InspectionValue::Object({{"kind", wire::InspectionValue::Unsigned(kind)},
														   {"id", wire::InspectionValue::Unsigned(1)}})}})
				.Encode()));
		Rejected([&] { app::SimulationEditSource(receipt); });
	}
}

void FirstPublicationAndStateFailures()
{
	CommandScheduler scheduler;
	scheduler.Start();
	app::ManualTransaction edit;
	edit.commands = {TakeControl{ControlChannel::Head}};
	auto packet = app::EncodeSimulationEdit(edit, 1, 1, 1, Observation(scheduler));
	Require(wire::DecodeSimulationTransaction(packet.message, 1).target.frame == FrameId{1},
			"A fresh scheduler must accept the first command publication");
	Rejected([&] { app::EncodeSimulationEdit(edit, 1, 1, 1, {}); });
	constexpr auto largeFrame = (std::uint64_t{1} << 53) + 7;
	Apply(scheduler, largeFrame);
	packet = app::EncodeSimulationEdit(edit, 1, 1, 1, Observation(scheduler));
	Require(wire::DecodeSimulationTransaction(packet.message, 1).target.frame == FrameId{largeFrame + 1},
			"Automatic target lost integer precision beyond the floating-point exact range");
	Apply(scheduler, std::numeric_limits<std::uint64_t>::max());
	Rejected([&] { app::EncodeSimulationEdit(edit, 1, 1, 1, Observation(scheduler)); });
	scheduler.Close();
	Rejected([&] { app::EncodeSimulationEdit(edit, 1, 1, 1, Observation(scheduler)); });
	wire::Message malformed;
	malformed.fields.push_back(wire::Field::Bytes(wire::tag::Document, std::array{std::byte{0xff}}));
	Rejected([&] { app::EncodeSimulationEdit(edit, 1, 1, 1, malformed); });
	wire::Message wrongType;
	wrongType.fields.push_back(wire::Field::Bytes(
		wire::tag::Document, wire::InspectionValue::Object({{"closed", wire::InspectionValue::Boolean(false)},
															{"frame_id", wire::InspectionValue::Signed(5)}})
								 .Encode()));
	Rejected([&] { app::EncodeSimulationEdit(edit, 1, 1, 1, wrongType); });

	packet = app::EncodeSimulationEdit(app::ReleaseManualInputs{}, 21, 4, 0, {});
	Require(packet.message.opcode == static_cast<std::uint16_t>(wire::OpCode::ReleaseControl) &&
				!packet.message.Find(wire::tag::ApplyFrame) && !packet.message.Find(wire::tag::TransactionId),
			"Immediate source retirement incorrectly depends on observed frames");
	Rejected([&] { app::EncodeSimulationEdit(app::ReleaseManualInputs{}, 0, 4, 0, {}); });
}

void GameExtensionObservation()
{
	CommandScheduler scheduler;
	scheduler.Start();
	Apply(scheduler, 1);
	const auto legacy = app::ReadObservedSetup(Observation(scheduler));
	Require(!legacy.appTrackersKnown && !legacy.appBodyExtensions, "Absent extension reports must remain unknown");
	for (unsigned mask = 0; mask < 8; ++mask)
	{
		const std::array enabled{(mask & 1) != 0, (mask & 2) != 0, (mask & 4) != 0};
		wire::Message message;
		message.fields.push_back(wire::Field::Bytes(
			wire::tag::Document,
			wire::SimulationState(scheduler.Snapshot(), false, mask != 0, {}, {}, enabled).Encode()));
		const auto observed = app::ReadObservedSetup(message);
		Require(observed.appTrackersKnown && observed.appTrackers == (mask != 0) &&
					observed.appBodyExtensions == enabled,
				"Game extension flags changed across inspection encoding and decoding");
	}
}

void ExpandedObservedTrackers()
{
	for (const auto count : {0u, 64u})
	{
		CommandConfiguration config;
		config.tracking.trackers.clear();
		for (unsigned index = 0; index < count; ++index)
			config.tracking.trackers.push_back({{100 + index}, TrackerRole::Unassigned});
		CommandScheduler scheduler;
		scheduler.Start(0, config);
		Apply(scheduler, 1);
		const auto observed = app::ReadObservedSetup(Observation(scheduler));
		Require(observed.values.tracking.trackers.size() == count, "Observed setup changed tracker inventory size");
		if (count)
			Require(observed.values.tracking.trackers.back().identity.value == 163,
					"Observed setup truncated tracker identities");
	}
}
} // namespace

int main()
{
	try
	{
		GameExtensionObservation();
		ExpandedObservedTrackers();
		ObservedFrameScheduling();
		DirectPiloting();
		PreserveObservedConfiguration();
		FirstPublicationAndStateFailures();
		SourceKindRange();
		std::cout << "PASS simulation edits\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
