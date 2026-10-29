#include "../TestSupport.h"
#include "Pch.h"
#include "automation/Recording.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include "runtime/Session.h"
#include "simulation/BodyModel.h"
#include "simulation/CommandScheduler.h"

#include <iostream>

namespace simxr::tests
{
static constexpr CommandSource s_device{CommandSourceKind::Device, 17};

class LiveFixture
{
public:
	CommandScheduler scheduler;
	LiveInputClock::time_point now{std::chrono::seconds(1)};
	LiveInputSample sample;
	std::uint64_t frame = 0;

	LiveFixture()
	{
		scheduler.Start();
		CommandTransaction claim;
		claim.id = 1;
		claim.source = s_device;
		claim.target.frame = FrameId{1};
		claim.commands = {TakeControl{ControlChannel::Views}, TakeControl{ControlChannel::Head},
						  TakeControl{ControlChannel::LeftPose}, TakeControl{ControlChannel::LeftControls},
						  TakeControl{ControlChannel::RightControls}};
		Require(scheduler.Submit(claim).result == CommandResult::Queued, "Queue device ownership");
		Tick();
		sample.connection = 23;
		sample.head.flags = TrackedPoseFlags;
		sample.hands[0].pose.flags = TrackedPoseFlags;
		sample.hands[0].aim = PoseSample{};
		sample.hands[0].aim->flags = ValidPoseFlags;
	}

	CommandResult Send()
	{
		++sample.sequence;
		sample.observedTime = {Duration{static_cast<std::int64_t>(sample.sequence)}};
		sample.targetTime = {Duration{1000 + static_cast<std::int64_t>(sample.sequence)}};
		return scheduler.SubmitLiveInput(s_device, sample, now);
	}

	std::shared_ptr<const AppliedCommandSnapshot> Tick()
	{
		++frame;
		return scheduler.Apply({frame}, {Duration{static_cast<std::int64_t>(frame)}},
							   {Duration{1000 + static_cast<std::int64_t>(frame)}}, {}, {}, now);
	}
};

static void BodyObservations()
{
	for (const auto set :
		 {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA, BodyJointSet::WithoutArmsBD, BodyJointSet::FullBodyBD})
	{
		LiveInputSlot slot;
		LiveInputSlot::Channels owned{};
		owned[static_cast<std::size_t>(ControlChannel::Body)] = true;
		LiveInputSample sample;
		sample.connection = sample.sequence = 1;
		sample.observedTime = {Duration{100}};
		sample.targetTime = {Duration{110}};
		BodyObservation body;
		body.jointSet = set;
		body.requestedTime = sample.targetTime;
		body.located = true;
		const bool fb = set == BodyJointSet::UpperBodyFB || set == BodyJointSet::FullBodyMETA;
		if (fb)
		{
			body.active = true;
			body.confidence = .75f;
			body.returnedTime = RuntimeTime{Duration{105}};
			body.skeletonRevision = 0;
			body.skeleton.emplace();
		}
		else
			body.allJointPosesTracked = false;
		for (const auto joint : BodyModel::JointMap(set))
		{
			const auto index = static_cast<std::size_t>(joint);
			body.joints[index] = ObservedBodyJoint{IdentityPose(), ValidPoseFlags};
			if (fb)
				(*body.skeleton)[index] = SkeletonJoint{joint, {}, IdentityPose()};
		}
		const LiveInputClock::time_point now{std::chrono::seconds(1)};
		Require(slot.Write(s_device, sample, owned, now) == CommandResult::Conflict,
				"Body owner may not silently omit its provider");
		sample.body = body;
		Require(slot.Write(s_device, sample, {}, now) == CommandResult::Conflict, "Unowned physical body was accepted");
		Require(slot.Write(s_device, sample, owned, now) == CommandResult::Queued,
				"Body-only live ownership was rejected");
		auto malformed = sample;
		malformed.sequence++;
		malformed.body->requestedTime.value += Duration{1};
		Require(slot.Write(s_device, malformed, owned, now) == CommandResult::Invalid && slot.Status().sequence == 1,
				"Mismatched body prediction changed live state");
		const auto applied = slot.Prepare({1}, {}, owned, now);
		Require(applied && applied->transaction.commands.size() == 1 &&
					std::get<SetBodyControls>(applied->transaction.commands[0]).observation->requestedTime ==
						sample.targetTime,
				"Live body changed its sample time or affected an unowned channel");
		const auto expired = slot.Prepare({2}, {}, owned, now + std::chrono::milliseconds(51));
		Require(expired.has_value(), "Expired body retained active observations");
		const auto& stale = *std::get<SetBodyControls>(expired->transaction.commands[0]).observation;
		for (const auto& joint : stale.joints)
			Require(!joint || !joint->flags, "Stale body joint remained valid");
		Require(stale.requestedTime == body.requestedTime && stale.returnedTime == body.returnedTime &&
					stale.skeletonRevision == body.skeletonRevision &&
					(fb ? stale.active == false : stale.allJointPosesTracked == false),
				"Expiry lost body identity/time or retained provider activity");
		Require(!slot.Prepare({3}, {}, owned, now + std::chrono::milliseconds(52)), "Expiry repeated body commands");
		CommandState state;
		CommandConfiguration baseline;
		baseline.tracking.body.preset = BodyPreset::Seated;
		CommandTransaction claim;
		claim.source = s_device;
		claim.commands = {TakeControl{ControlChannel::Body}, SetBodyControls{{}, body}};
		Require(state.Apply(claim, baseline, {Duration{110}}) == CommandResult::Applied, "Apply live body ownership");
		state.ReleaseSource(s_device);
		Require(!state.Merge(baseline).tracking.bodyObservation &&
					state.Merge(baseline).tracking.body.preset == BodyPreset::Seated,
				"Release did not restore authored body baseline");
	}
}

static void TrackerObservations()
{
	LiveFixture fixture;
	fixture.sample.trackers.emplace();
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
	{
		TrackerDevice device{{100 + index}, TrackerRole::Unassigned};
		device.poseOverride = XrPosef{{0, 0, 0, 1}, {float(index), 1, 0}};
		device.poseFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
		fixture.sample.trackers->push_back(device);
	}
	Require(fixture.Send() == CommandResult::Conflict, "Unowned live tracker inventory was accepted");
	CommandTransaction claim;
	claim.id = 2;
	claim.source = s_device;
	claim.target.frame = FrameId{fixture.frame + 1};
	claim.commands = {TakeControl{ControlChannel::Trackers}};
	Require(fixture.scheduler.Submit(claim).result == CommandResult::Queued, "Queue tracker ownership");
	fixture.Tick();
	Require(fixture.Send() == CommandResult::Queued, "Owned live tracker inventory was rejected");
	const auto applied = fixture.Tick();
	Require(applied->configuration.tracking.trackers.size() == 64 &&
				applied->configuration.tracking.trackers.back().identity.value == 163 &&
				applied->configuration.tracking.trackers.back().poseFlags == XR_SPACE_LOCATION_ORIENTATION_VALID_BIT,
			"Live inventory or component validity was lost");
	fixture.sample.trackers->back().identity.value = 100;
	Require(fixture.Send() == CommandResult::Invalid, "Duplicate live identities were accepted");
	fixture.sample.trackers->back().identity.value = 163;
	fixture.sample.trackers->back().anchor = BodyJoint::Hips;
	Require(fixture.Send() == CommandResult::Invalid, "Physical input accepted a simulated joint attachment");
	fixture.sample.trackers->back().anchor.reset();
	fixture.sample.trackers.reset();
	Require(fixture.Send() == CommandResult::Conflict, "Owned tracker channel silently omitted observations");
	fixture.now += std::chrono::milliseconds(51);
	const auto stale = fixture.Tick();
	Require(stale->configuration.tracking.trackers.size() == 64 &&
				stale->configuration.tracking.trackers.back().poseFlags == 0 &&
				!stale->configuration.tracking.trackers.back().tracked,
			"Stale tracker inventory retained valid tracking");
	LiveInputSlot slot;
	LiveInputSlot::Channels owned{};
	owned[static_cast<std::size_t>(ControlChannel::Trackers)] = true;
	fixture.sample.trackers.emplace();
	Require(slot.Write(s_device, fixture.sample, owned, fixture.now) == CommandResult::Queued,
			"Tracker-only empty inventory was rejected");
	const auto empty = slot.Prepare({1}, {}, owned, fixture.now);
	Require(empty && empty->transaction.commands.size() == 1 &&
				std::get<SetTrackers>(empty->transaction.commands.front()).value.empty(),
			"Tracker-only empty inventory did not publish removal");
}

static void TrackerTransitions()
{
	LiveInputSlot slot;
	LiveInputSlot::Channels owned{};
	owned[static_cast<std::size_t>(ControlChannel::Trackers)] = true;
	LiveInputSample sample;
	sample.connection = 41;
	sample.trackers = std::vector<TrackerDevice>{TrackerDevice{{71}, TrackerRole::Unassigned}};
	sample.trackers->front().poseOverride = XrPosef{{0, 0, 0, 1}, {0, 1, 0}};
	const auto now = LiveInputClock::time_point{std::chrono::seconds(1)};
	const auto send = [&] {
		++sample.sequence;
		sample.observedTime = {Duration{static_cast<std::int64_t>(sample.sequence)}};
		sample.targetTime = sample.observedTime;
		return slot.Write(s_device, sample, owned, now);
	};
	const auto read = [&] {
		return slot.Prepare({1}, {}, owned, now);
	};
	Require(send() == CommandResult::Queued, "Queue initial tracker connection");
	sample.trackers->front().connected = false;
	Require(send() == CommandResult::Queued, "Queue brief tracker disconnect");
	sample.trackers->front().connected = true;
	sample.trackers->front().role = TrackerRole::Waist;
	Require(send() == CommandResult::Queued, "Queue tracker reconnection with new role");
	sample.trackers->front().poseFlags = 0;
	Require(send() == CommandResult::Queued, "Queue current tracking loss");
	for (std::uint64_t sequence = 1; sequence <= 3; ++sequence)
	{
		const auto outcome = read();
		Require(outcome.has_value(), "Tracker transition disappeared");
		const auto& device = std::get<SetTrackers>(outcome->transaction.commands.front()).value.front();
		Require(device.poseFlags == 0, "Queued topology resurrected lost tracking validity");
		Require(device.connected == (sequence != 2) &&
					device.role == (sequence == 3 ? TrackerRole::Waist : TrackerRole::Unassigned) &&
					outcome->liveInput->trackersObservedTime->value.count() == static_cast<std::int64_t>(sequence),
				"Tracker transition order or timestamp changed");
		if (sequence < 3)
		{
			const auto held = read();
			Require(held && held->liveInput->trackersObservedTime == outcome->liveInput->trackersObservedTime,
					"Tracker edge advanced before action synchronization");
		}
		slot.AcknowledgeControls();
	}
	Require(read().has_value(), "Latest tracker poses were not refreshed after transition acknowledgement");
	for (std::size_t index = 0; index < 16; ++index)
	{
		sample.trackers->front().connected = !sample.trackers->front().connected;
		Require(send() == CommandResult::Queued, "Bounded tracker queue filled too early");
	}
	sample.trackers->front().connected = !sample.trackers->front().connected;
	Require(send() == CommandResult::Capacity && slot.Status().phase == LiveInputPhase::Overflow,
			"Tracker overflow silently discarded a transition");
	const auto expired = read();
	Require(expired && std::get<SetTrackers>(expired->transaction.commands.front()).value.front().poseFlags == 0,
			"Tracker overflow retained valid physical poses");
}

static void ReacquiredObservationChannels()
{
	for (const auto channel : {ControlChannel::Views, ControlChannel::Trackers})
	{
		LiveFixture fixture;
		CommandTransaction claim;
		claim.id = 2;
		claim.source = s_device;
		claim.target.frame = FrameId{fixture.frame + 1};
		claim.commands = {TakeControl{channel}};
		Require(fixture.scheduler.Submit(claim).result == CommandResult::Queued, "Claim observed channel");
		fixture.Tick();
		if (channel == ControlChannel::Trackers)
		{
			TrackerDevice device{{701}, TrackerRole::Unassigned};
			device.poseOverride = XrPosef{{0, 0, 0, 1}, {7, 1, 0}};
			fixture.sample.trackers = std::vector<TrackerDevice>{device};
		}
		else
		{
			fixture.sample.eyeViews.emplace();
			fixture.sample.eyeViews->spaceRevision = 7;
		}
		Require(fixture.Send() == CommandResult::Queued, "Queue observation before ownership release");
		claim.id = 3;
		claim.target.frame = FrameId{fixture.frame + 1};
		claim.commands = {ReleaseControl{channel}, TakeControl{channel}};
		Require(fixture.scheduler.Submit(claim).result == CommandResult::Queued, "Queue explicit release/reacquire");
		const auto boundary = fixture.Tick();
		Require(!fixture.scheduler.LiveInputState().owner &&
					std::none_of(boundary->outcomes.begin(), boundary->outcomes.end(),
								 [](const CommandOutcome& outcome) { return outcome.liveInput.has_value(); }),
				"Old live observations crossed an explicit ownership release");
		if (fixture.sample.trackers)
			fixture.sample.trackers->front().identity.value = 702;
		else
			fixture.sample.eyeViews->spaceRevision = 8;
		Require(fixture.Send() == CommandResult::Queued, "Fresh observation rejected after reacquisition");
		const auto fresh = fixture.Tick();
		if (fixture.sample.trackers)
			Require(fresh->configuration.tracking.trackers.front().identity.value == 702,
					"Retired tracker event displaced the fresh inventory");
		else
			Require(fresh->configuration.tracking.eyeViews->spaceRevision == 8,
					"Retired eye observation displaced fresh geometry");
	}
}

static void EyeObservations()
{
	LiveFixture fixture;
	fixture.sample.eyeViews.emplace();
	fixture.sample.eyeViews->spaceRevision = 3;
	fixture.sample.eyeViews->poses[0].flags = TrackedPoseFlags;
	fixture.sample.eyeViews->poses[0].pose.position.x = -.04f;
	Require(fixture.Send() == CommandResult::Queued, "Queue physical eye observation");
	const auto applied = fixture.Tick();
	Require(applied->configuration.tracking.eyeViews->poses[0].pose.position.x == -.04f &&
				applied->configuration.tracking.eyeViews->poses[1].flags == 0,
			"Apply independent observed eye validity");
	fixture.sample.eyeViews->fovs[0].angleLeft = 2;
	Require(fixture.Send() == CommandResult::Invalid, "Reject malformed live FOV before replacing the mailbox");
	fixture.now += std::chrono::milliseconds(51);
	const auto stale = fixture.Tick();
	Require(stale->configuration.tracking.eyeViews && stale->configuration.tracking.eyeViews->poses[0].flags == 0 &&
				stale->configuration.tracking.eyeViews->spaceRevision == 3,
			"Stale observations clear validity while retaining their projection and calibration context");
}

static void FocusSuspension()
{
	LiveFixture fixture;
	fixture.sample.controls[0].primary = true;
	Require(fixture.Send() == CommandResult::Queued, "Queue pre-loss press");
	Require(fixture.Tick()->configuration.input.controllers[0].primary, "Publish held press before focus loss");
	fixture.sample.controls[0].secondary = true;
	fixture.Send();
	fixture.sample.controlsActive = false;
	Require(fixture.Send() == CommandResult::Queued, "Suspend while controls await action synchronization");
	fixture.sample.controlsActive = true;
	fixture.sample.controls[0].primary = false;
	fixture.Send();
	const auto released = fixture.Tick();
	Require(!released->configuration.input.controllers[0].primary &&
				!released->configuration.input.controllers[0].secondary,
			"Suspension flushes pending and unacknowledged presses even if resumed before publication");
	Require(!fixture.Tick()->configuration.input.controllers[0].secondary,
			"Neutral boundary waits for action synchronization");
	fixture.scheduler.AcknowledgeLiveInput();
	Require(fixture.Tick()->configuration.input.controllers[0].secondary,
			"Post-resume press follows the acknowledged neutral boundary");
	fixture.sample.controlsActive = false;
	fixture.Send();
	fixture.Send();
	Require(!fixture.Tick()->configuration.input.controllers[0].secondary,
			"Repeated inactive samples cannot reintroduce their raw held controls");
}

static void ProfileTransitions()
{
	LiveFixture fixture;
	fixture.sample.controls[0].physicalProfile = "/interaction_profiles/htc/vive_controller";
	Require(fixture.Send() == CommandResult::Queued, "Queue physical Vive profile");
	fixture.sample.controls[0].physicalProfile = "/interaction_profiles/valve/index_controller";
	Require(fixture.Send() == CommandResult::Queued, "Queue physical Index profile");
	Require(fixture.Tick()->configuration.input.controllers[0].physicalProfile ==
				"/interaction_profiles/htc/vive_controller",
			"Preserve a profile transition even without a button edge");
	fixture.scheduler.AcknowledgeLiveInput();
	Require(fixture.Tick()->configuration.input.controllers[0].physicalProfile ==
				"/interaction_profiles/valve/index_controller",
			"Publish next physical profile after action synchronization");
	fixture.sample.controls[0].physicalProfile = "/not_a_profile";
	Require(fixture.Send() == CommandResult::Invalid, "Reject invalid physical profile namespace");
	fixture.sample.controls[0].physicalProfile = "/interaction_profiles/" + std::string(XR_MAX_PATH_LENGTH, 'a');
	Require(fixture.Send() == CommandResult::Invalid, "Bound physical profile strings before queuing");
}

static void TriggerClickEdges()
{
	LiveFixture fixture;
	fixture.sample.controls[0].trigger = .1f;
	fixture.sample.controls[0].triggerClick = true;
	Require(fixture.Send() == CommandResult::Queued, "Queue physical click independent of analog pressure");
	fixture.sample.controls[0].triggerClick = false;
	Require(fixture.Send() == CommandResult::Queued, "Queue physical click release");
	Require(fixture.Tick()->configuration.input.controllers[0].triggerClick == true,
			"Preserve short physical trigger press");
	fixture.scheduler.AcknowledgeLiveInput();
	Require(fixture.Tick()->configuration.input.controllers[0].triggerClick == false,
			"Preserve physical trigger release without analog crossing");
}

static void ObservedTap()
{
	LiveFixture fixture;
	fixture.sample.controls[0].primary = true;
	fixture.sample.controls[0].trigger = .8f;
	Require(fixture.Send() == CommandResult::Queued, "Queue observed press");
	fixture.sample.controls[0].primary = false;
	fixture.sample.controls[0].trigger = 0;
	fixture.sample.head.pose.position.x = 3;
	Require(fixture.Send() == CommandResult::Queued, "Queue observed release");
	const auto press = fixture.Tick();
	Require(press->configuration.input.controllers[0].primary &&
				press->configuration.input.controllers[0].trigger == .8f,
			"Coalescing preserves digital and thresholded analog press");
	Require(press->configuration.tracking.head.pose.position.x == 3, "Pose mailbox retains the newest observation");
	Require(fixture.Tick()->configuration.input.controllers[0].primary,
			"Frame progress cannot consume an unobserved edge");
	fixture.scheduler.AcknowledgeLiveInput();
	const auto release = fixture.Tick();
	Require(!release->configuration.input.controllers[0].primary &&
				release->configuration.input.controllers[0].trigger == 0,
			"Release follows acknowledged press");
	Require(fixture.scheduler.LiveInputState().coalesced == 1, "Coalescing is observable");
}

static void Rollback()
{
	LiveFixture fixture;
	fixture.sample.controls[0].primary = true;
	fixture.Send();
	fixture.sample.controls[0].primary = false;
	fixture.Send();
	bool rejected = false;
	try
	{
		fixture.scheduler.Apply(
			{2}, {Duration{2}}, {Duration{1002}}, {}, [](const auto&) { throw std::bad_alloc{}; }, fixture.now);
	}
	catch (const std::bad_alloc&)
	{
		rejected = true;
	}
	Require(rejected && fixture.Tick()->configuration.input.controllers[0].primary,
			"Failed frame reservation cannot consume a pending press");
	fixture.scheduler.AcknowledgeLiveInput();
	Require(!fixture.Tick()->configuration.input.controllers[0].primary, "Rollback preserves the following release");
}

static void Faults()
{
	LiveFixture stale;
	stale.sample.controls[0].primary = true;
	stale.Send();
	stale.Tick();
	stale.now += std::chrono::milliseconds(50);
	const auto expired = stale.Tick();
	Require(!expired->configuration.input.controllers[0].primary && expired->configuration.tracking.head.flags == 0 &&
				expired->configuration.tracking.controllers[0].aim->flags == 0,
			"Expiry clears held controls and both pose validities");
	Require(stale.scheduler.LiveInputState().phase == LiveInputPhase::Stale && stale.Send() == CommandResult::Cancelled,
			"Stale source requires explicit reattachment");
	Require(stale.Tick()->outcomes.empty(), "Suspension must not append duplicate recording events");
	LiveFixture overflow;
	for (unsigned index = 0; index < 16; ++index)
	{
		overflow.sample.controls[0].primary = (index % 2) == 0;
		Require(overflow.Send() == CommandResult::Queued, "Queue bounded transition");
	}
	overflow.sample.controls[0].primary = true;
	Require(overflow.Send() == CommandResult::Capacity, "Overflow is reported instead of merging edges");
	Require(!overflow.Tick()->configuration.input.controllers[0].primary &&
				overflow.scheduler.LiveInputState().phase == LiveInputPhase::Overflow,
			"Overflow suspends and releases source");
	Require(overflow.Send() == CommandResult::Cancelled, "Overflow cannot silently resume");
}

static void PausedExpiry()
{
	LiveFixture fixture;
	fixture.scheduler.StartRecording({}, {});
	fixture.sample.controls[0].primary = true;
	fixture.Send();
	fixture.Tick();
	CommandTransaction pending;
	pending.id = 2;
	pending.source = s_device;
	pending.target.frame = FrameId{100};
	pending.commands = {TakeControl{ControlChannel::LeftControls}};
	Require(fixture.scheduler.Submit(pending).result == CommandResult::Queued, "Queue future device command");
	Require(!fixture.scheduler.ExpireLiveInput(fixture.now)[3], "Fresh source retains ownership");
	fixture.now += std::chrono::milliseconds(50);
	Require(fixture.scheduler.ExpireLiveInput(fixture.now)[3], "Expire without a frame boundary");
	Require(fixture.scheduler.LiveInputState().phase == LiveInputPhase::Disconnected,
			"Expiry clears attachment and pending edges");
	Require(fixture.Send() == CommandResult::Conflict, "Expired source cannot resume without ownership");
	const auto total = fixture.scheduler.Recorder().Snapshot().total;
	fixture.scheduler.ExpireLiveInput(fixture.now);
	Require(fixture.scheduler.Recorder().Snapshot().total == total, "Expiry is recorded only once");
	fixture.frame = 99;
	Require(!fixture.Tick()->configuration.input.controllers[0].primary && fixture.Send() == CommandResult::Conflict,
			"Queued ownership cannot reclaim an expired source");

	Instance instance(reinterpret_cast<XrInstance>(981), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(982), instance);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
			"Start idle live-input session");
	CommandTransaction claim;
	claim.id = 1;
	claim.source = s_device;
	claim.target.frame = FrameId{1};
	ControllerControls controls;
	controls.primary = true;
	claim.commands = {TakeControl{ControlChannel::LeftControls}, SetControllerControls{0, controls}};
	Require(session.Commands().Submit(claim).result == CommandResult::Queued, "Claim session controls");
	session.Commands().Apply({1}, {}, timing.epoch, {});
	Require(session.PublishInput({}) == XR_SUCCESS && session.Input().Snapshot().controllers[0].primary,
			"Publish held control before game stops advancing");
	auto sample = fixture.sample;
	sample.maxAge = std::chrono::milliseconds(20);
	const auto before = session.Snapshot();
	Require(session.SubmitLiveInput({}, s_device, sample) == CommandResult::Conflict, "Reject stale generation");
	Require(session.SubmitLiveInput(before.generation, s_device, sample) == CommandResult::Queued,
			"Start session-owned expiry worker");
	const auto deadline = LiveInputClock::now() + std::chrono::seconds(3);
	while (session.Input().Snapshot().controllers[0].primary && LiveInputClock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	Require(!session.Input().Snapshot().controllers[0].primary, "Watchdog releases controls without game frames");
	Require(session.Snapshot().lastIssued.id == before.lastIssued.id && session.Snapshot().time == before.time,
			"Expiry does not advance simulation time or issue a frame");
	Require(session.RequestExit(timing.epoch) == XR_SUCCESS && session.End(timing.epoch) == XR_SUCCESS,
			"End joins the idle watchdog");
	Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
			"Restart session after watchdog shutdown");
	const auto restarted = session.Snapshot();
	Require(restarted.generation != before.generation &&
				session.SubmitLiveInput(before.generation, s_device, sample) == CommandResult::Conflict,
			"Old generation cannot start input after restart");
	Require(session.Commands().Submit(claim).result == CommandResult::Queued, "Reclaim controls after restart");
	session.Commands().Apply({1}, {}, timing.epoch, {});
	Require(session.PublishInput({}) == XR_SUCCESS && session.Input().Snapshot().controllers[0].primary,
			"Publish restarted held control");
	Require(session.SubmitLiveInput(restarted.generation, s_device, sample) == CommandResult::Queued,
			"Restart the session-owned worker");
	const auto restartDeadline = LiveInputClock::now() + std::chrono::seconds(3);
	while (session.Input().Snapshot().controllers[0].primary && LiveInputClock::now() < restartDeadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	Require(!session.Input().Snapshot().controllers[0].primary, "Restarted watchdog releases controls");
	session.Close();
	Require(session.SubmitLiveInput(before.generation, s_device, sample) == CommandResult::Closed,
			"Closed session rejects samples after worker join");
}

static void ClockRevision()
{
	LiveFixture fixture;
	fixture.sample.clockRevision = 7;
	fixture.sample.controls[0].primary = true;
	Require(fixture.Send() == CommandResult::Queued, "Admit live sample under clock revision seven");
	const auto applied = fixture.scheduler.Apply({2}, {Duration{2}}, {Duration{1002}}, {}, {}, fixture.now, 8);
	Require(!applied->configuration.input.controllers[0].primary && applied->configuration.tracking.head.flags == 0 &&
				fixture.scheduler.LiveInputState().phase == LiveInputPhase::Stale,
			"Rebase between admission and publication neutralizes stale live input");
	Require(applied->outcomes.back().liveInput->clockRevision == 7,
			"Neutralization retains original clock correlation");
	fixture.sample.clockRevision = 8;
	Require(fixture.Send() == CommandResult::Conflict, "New clock cannot silently replace old queued transitions");
}

static void IdentityAndRecording()
{
	LiveFixture fixture;
	fixture.sample.clockRevision = 19;
	fixture.sample.eyeViews.emplace();
	fixture.sample.eyeViews->spaceRevision = 11;
	fixture.sample.eyeViews->poses[0].flags = TrackedPoseFlags;
	fixture.sample.eyeViews->poses[0].pose.position.x = -.03f;
	fixture.scheduler.StartRecording({}, {});
	fixture.sample.controls[0].trigger = .9f;
	Require(fixture.Send() == CommandResult::Queued, "Accept device sample");
	auto alien = fixture.sample;
	++alien.connection;
	++alien.sequence;
	Require(fixture.scheduler.SubmitLiveInput(s_device, alien, fixture.now) == CommandResult::Conflict,
			"Different attachment cannot replace an active slot");
	fixture.Tick();
	const auto recording = fixture.scheduler.Recorder().Snapshot();
	Require(recording.total == 1 && recording.transactions[0].transaction.source == s_device,
			"Record actually applied device commands with source identity");
	control::Message page;
	page.fields = {control::Field::Bytes(control::tag::Document, control::SimulationRecording(recording).Encode())};
	const auto decoded = automation::DecodeRecordingPage(page);
	Require(decoded.recording.transactions[0].liveInput &&
				decoded.recording.transactions[0].liveInput->connection == fixture.sample.connection &&
				decoded.recording.transactions[0].liveInput->clockRevision == 19,
			"Native recording reader retains live source correlation");
	const auto& recordedCommands = decoded.recording.transactions[0].commands;
	const auto eyes = std::find_if(recordedCommands.begin(), recordedCommands.end(),
								   [](const auto& command) { return std::holds_alternative<SetEyeViews>(command); });
	Require(eyes != recordedCommands.end() && std::get<SetEyeViews>(*eyes).value->spaceRevision == 11 &&
				std::get<SetEyeViews>(*eyes).value->poses[0].pose.position.x == -.03f,
			"Applied eye observations survive recording and native replay decoding");
	fixture.scheduler.ReleaseSource(s_device);
	Require(fixture.scheduler.LiveInputState().phase == LiveInputPhase::Disconnected &&
				fixture.Send() == CommandResult::Conflict,
			"Source release clears pending input and prevents unowned forwarding");
	fixture.scheduler.Close();
	Require(fixture.Send() == CommandResult::Closed, "Closed scheduler rejects live input");
}
} // namespace simxr::tests

int main()
{
	try
	{
		simxr::tests::BodyObservations();
		simxr::tests::TrackerObservations();
		simxr::tests::TrackerTransitions();
		simxr::tests::ReacquiredObservationChannels();
		simxr::tests::EyeObservations();
		simxr::tests::FocusSuspension();
		simxr::tests::ProfileTransitions();
		simxr::tests::TriggerClickEdges();
		simxr::tests::ObservedTap();
		simxr::tests::Rollback();
		simxr::tests::Faults();
		simxr::tests::PausedExpiry();
		simxr::tests::IdentityAndRecording();
		simxr::tests::ClockRevision();
		std::cout << "Live input scheduler checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
