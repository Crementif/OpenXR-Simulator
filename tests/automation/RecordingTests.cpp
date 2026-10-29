#include "../TestSupport.h"
#include "automation/Recording.h"
#include "control/ControlSchema.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include "runtime/Session.h"
#include "simulation/CommandRecorder.h"
#include "simulation/CommandState.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace
{
using namespace simxr;
namespace wire = control;
namespace rigs = automation;

template <class Operation> void Rejected(Operation operation, const char* reason)
{
	try
	{
		operation();
	}
	catch (const std::exception&)
	{
		return;
	}
	throw std::runtime_error(reason);
}
wire::Message Page(const CommandRecorder& recorder, std::size_t offset, std::size_t count)
{
	const auto recording = recorder.Snapshot(offset, count);
	wire::Message response;
	response.fields = {wire::Field::Bytes(wire::tag::Document, wire::SimulationRecording(recording).Encode()),
					   wire::Field::U64(wire::tag::Offset, offset),
					   wire::Field::U64(wire::tag::Length, recording.transactions.size()),
					   wire::Field::Bool(wire::tag::End, offset + recording.transactions.size() == recording.total)};
	return response;
}
static BodyObservation RecordedBody()
{
	BodyObservation body;
	body.jointSet = BodyJointSet::UpperBodyFB;
	body.located = true;
	body.active = true;
	body.confidence = .75f;
	body.requestedTime = {Duration{9007199254740993LL}};
	body.returnedTime = RuntimeTime{Duration{9007199254740991LL}};
	body.skeletonRevision = 0;
	body.skeleton.emplace();
	for (const auto joint : BodyModel::JointMap(body.jointSet))
	{
		const auto index = static_cast<std::size_t>(joint);
		body.joints[index] = ObservedBodyJoint{IdentityPose(), ValidPoseFlags};
		body.joints[index]->pose.position.x = static_cast<float>(index);
		(*body.skeleton)[index] = SkeletonJoint{joint, {}, IdentityPose()};
	}
	return body;
}
static void BodyRecordingSchema()
{
	CommandRecorder recorder;
	recorder.Start({});
	Require(recorder.Snapshot().version == 3, "Authored recordings retain schema 3");
	CommandOutcome outcome;
	outcome.result = CommandResult::Applied;
	outcome.appliedFrame = FrameId{1};
	outcome.transaction.id = 1;
	outcome.transaction.commands = {SetBodyControls{{}, RecordedBody()}};
	outcome.appliedRuntimeTime = RuntimeTime{Duration{9007199254741000LL}};
	Rejected([&]
	{
		recorder.ObservePrepared(std::span{&outcome, 1}, [&](bool)
		{
			Require(recorder.Snapshot().version == 3 && recorder.Snapshot().total == 0,
				"Recorder cannot expose a partial schema upgrade during reservation");
			throw std::bad_alloc{};
		});
	}, "Failed observation publication must propagate");
	Require(recorder.Snapshot().version == 3 && recorder.Snapshot().total == 0,
		"Failed publication changed recording version or transactions");
	Require(recorder.Observe(outcome), "Record body command");
	recorder.Stop();
	const auto recorded = recorder.Snapshot();
	Require(recorded.version == 4 && recorded.total == 1, "Body command promotes recording schema atomically");
	const auto page = rigs::DecodeRecordingPage(Page(recorder, 0, 16));
	const auto& command = std::get<SetBodyControls>(page.recording.transactions[0].commands[0]);
	Require(page.recording.version == 4 && command.observation &&
		wire::EncodeSimulationCommand(command).value == wire::EncodeSimulationCommand(outcome.transaction.commands[0]).value,
		"Schema-4 command survives recording transport without losing body metadata");
	rigs::RecordingCollection collection;
	collection.Append(page);
	Require(collection.Complete() && collection.Recording().version == 4, "Collect complete schema-4 take");
	recorder.Start({}, true);
	Require(recorder.Observe(outcome), "Record body in draining window");
	recorder.Acknowledge(1);
	Require(recorder.Snapshot().version == 4 && recorder.Snapshot().floor == 1 &&
			recorder.Snapshot().transactions.empty(), "Acknowledging body data downgraded its archive schema");
	Require(recorder.Observe(outcome), "Reuse retired body slot");
	const auto retained = recorder.Snapshot();
	Require(retained.total == 2 && retained.floor == 1 && retained.transactions.size() == 1 &&
			wire::EncodeSimulationCommand(retained.transactions[0].transaction.commands[0]).value ==
			wire::EncodeSimulationCommand(outcome.transaction.commands[0]).value &&
			retained.transactions[0].appliedRuntimeTime == outcome.appliedRuntimeTime,
			"Draining changed observed body metadata or replay clock anchor");
	recorder.Stop();
	CommandRecordingOrigin origin;
	origin.configuration.tracking.bodyObservation = RecordedBody();
	origin.runtimeTime = RuntimeTime{Duration{9007199254741000LL}};
	recorder.Start(origin);
	recorder.Stop();
	Require(recorder.Snapshot().version == 4, "Observed recording origin selects schema 4");
	const auto initial = rigs::DecodeRecordingPage(Page(recorder, 0, 16)).recording;
	const auto body = std::find_if(initial.initial.begin(), initial.initial.end(), [](const auto& value)
	{
		const auto* command = std::get_if<SetBodyControls>(&value);
		return command && command->observation;
	});
	Require(body != initial.initial.end() && std::get<SetBodyControls>(*body).observation->returnedTime ==
		origin.configuration.tracking.bodyObservation->returnedTime, "Recording origin preserves exact body sample time");
	Require(rigs::EditableRig(initial).find("9007199254740993") != std::string::npos,
		"Schema-4 editable export preserves exact observation time");
	Require(initial.originRuntimeTime == origin.runtimeTime && page.recording.transactions[0].runtimeTime == outcome.appliedRuntimeTime &&
		rigs::EditableRig(initial).find("observation_origin = \"9007199254741000\"") != std::string::npos,
		"Recorded runtime boundary is missing from replay export");
	auto missingAnchor = initial;
	missingAnchor.originRuntimeTime.reset();
	Rejected([&] { rigs::EditableRig(missingAnchor); }, "Unanchored observed replay must fail explicitly");
	auto invalid = initial;
	invalid.version = 3;
	Rejected([&] { rigs::ReplayInitialCommands(invalid); }, "Old-schema body origin must not replay silently");
	CommandRecorder full(1);
	full.Start({});
	CommandOutcome authored = outcome;
	authored.transaction.commands = {SetBodyControls{}};
	Require(full.Observe(authored) && !full.Observe(outcome) && full.Snapshot().version == 3,
		"Rejected overflow body command must not upgrade stored schema");
}
void CollectionIdentityAndRollback()
{
	CommandRecorder recorder;
	CommandConfiguration initial;
	initial.tracking.head.pose.position.x = 1.25f;
	const auto origin = std::numeric_limits<std::uint64_t>::max() - 100;
	recorder.Start(
		{std::numeric_limits<std::uint64_t>::max(), initial, {}, FrameId{origin}, SimulationTime{Duration{13}}});
	for (std::uint64_t index = 1; index <= 19; ++index)
	{
		CommandOutcome outcome;
		outcome.result = CommandResult::Applied;
		outcome.appliedFrame = FrameId{origin + index};
		outcome.transaction.id = index;
		outcome.transaction.commands = {SetGaze{{.125f, -.25f, true, false}}};
		Require(recorder.Observe(outcome), "Record outcome");
	}
	rigs::RecordingCollection collection;
	Rejected(
		[&]
		{
			collection.Append(rigs::DecodeRecordingPage(Page(recorder, 0, 16)));
		},
		"Active recording accepted");
	Require(collection.Offset() == 0, "Rejected active recording changed collection");
	recorder.Stop();
	collection.Append(rigs::DecodeRecordingPage(Page(recorder, 0, 16)));
	Require(collection.Offset() == 16 && !collection.Complete(), "First page prematurely completed collection");
	Rejected(
		[&]
		{
			(void)collection.Recording();
		},
		"Incomplete collection exposed for export");
	const auto last = rigs::DecodeRecordingPage(Page(recorder, 16, 16));
	for (int change = 0; change < 8; ++change)
	{
		auto corrupt = last;
		switch (change)
		{
		case 0:
			++corrupt.offset;
			break;
		case 1:
			++corrupt.recording.seed;
			break;
		case 2:
			++corrupt.recording.originTime;
			break;
		case 3:
			std::get<SetHeadPose>(corrupt.recording.initial.front()).value.pose.position.x = 9;
			break;
		case 4:
			corrupt.recording.overflow = true;
			break;
		case 5:
			corrupt.end = false;
			break;
		case 6:
			corrupt.recording.total = 18;
			break;
		case 7:
			corrupt.recording.initialScenarios[0] = true;
			break;
		}
		Rejected(
			[&]
			{
				collection.Append(std::move(corrupt));
			},
			"Inconsistent recording page accepted");
		Require(collection.Offset() == 16 && !collection.Complete(), "Rejected page partially changed collection");
	}
	{
		auto oversized = last;
		oversized.recording.transactions.front().phase.assign(64 * 1024 * 1024, 'x');
		Rejected(
			[&]
			{
				collection.Append(std::move(oversized));
			},
			"Recording phase text bypassed the collection byte bound");
		Require(collection.Offset() == 16 && !collection.Complete(),
				"Oversized metadata partially replaced the previous recording page");
	}
	collection.Append(last);
	const auto& recording = collection.Recording();
	Require(recording.seed == std::numeric_limits<std::uint64_t>::max() && recording.originFrame.value == origin &&
				recording.transactions.size() == 19 && recording.transactions.back().frame.value == origin + 19,
			"Full-width recording identity or final page lost");
	auto physical = recording;
	physical.version = 3;
	physical.transactions.front().id = 0;
	physical.transactions.front().liveInput =
		LiveInputCorrelation{std::numeric_limits<std::uint64_t>::max(),
							 7,
							 RuntimeTime{Duration{100}},
							 RuntimeTime{Duration{110}},
							 {RuntimeTime{Duration{90}}, RuntimeTime{Duration{100}}},
							 19};
	physical.transactions.front().liveInput->trackersObservedTime = RuntimeTime{Duration{80}};
	const auto physicalScript = rigs::EditableRig(physical);
	Require(physicalScript.find("-- Tracker observation runtime ns: 80") != std::string::npos,
			"Editable export discarded tracker observation time");
	Require(physicalScript.find("-- Physical input: connection 18446744073709551615; sequence 7; clock_revision 19") !=
					std::string::npos &&
				physicalScript.find("-- Original runtime ns: observed 100; target 110; controls 90, 100") !=
					std::string::npos &&
				physicalScript.find("-- Original frame " + std::to_string(origin + 1)) != std::string::npos,
			"Editable export discarded exact physical-input provenance");
	const auto script = rigs::EditableRig(recording);
	Require(script.find("frame_add(baseFrame, \"19\")") != std::string::npos &&
				script.find("kind = \"gaze\"") != std::string::npos && script.find("tonumber") == std::string::npos,
			"Export lost final transaction or used lossy frame arithmetic");
	Rejected(
		[&]
		{
			collection.Append(last);
		},
		"Completed recording accepted an extra page");
	auto malformed = Page(recorder, 0, 16);
	malformed.fields.back() = wire::Field::Bool(wire::tag::End, true);
	Rejected(
		[&]
		{
			(void)rigs::DecodeRecordingPage(malformed);
		},
		"Wrong end flag accepted");
	malformed.fields.front().value.pop_back();
	Rejected(
		[&]
		{
			(void)rigs::DecodeRecordingPage(malformed);
		},
		"Truncated document accepted");
}

void ReleaseEventExport()
{
	CommandRecorder recorder;
	recorder.Start({73, {}, {}, FrameId{10}});
	CommandOutcome held;
	held.result = CommandResult::Applied;
	held.transaction.id = 1;
	held.appliedFrame = FrameId{11};
	held.transaction.commands = {TakeControl{ControlChannel::LeftControls}, SetControllerControls{}};
	recorder.Observe(held);
	CommandOutcome release;
	release.result = CommandResult::Released;
	release.appliedFrame = FrameId{11};
	release.afterFrame = true;
	release.transaction.commands = {ReleaseControl{ControlChannel::LeftControls}};
	recorder.Observe(release);
	held.transaction.id = 2;
	held.appliedFrame = FrameId{13};
	recorder.Observe(held);
	recorder.Stop();
	auto recording = rigs::DecodeRecordingPage(Page(recorder, 0, 16)).recording;
	Require(recording.version == 3 && recording.transactions[1].afterFrame &&
				recording.transactions[1].phase == "released",
			"Decoder lost release timing");
	const auto script = rigs::EditableRig(recording);
	Require(script.find("recorded frame +1") < script.find("recorded frame +2") &&
				script.find("recorded frame +2") < script.find("kind = \"release_control\"") &&
				script.find("kind = \"release_control\"") < script.find("recorded frame +3"),
			"Editable replay omitted or mistimed source release");
	for (int change = 0; change < 5; ++change)
	{
		auto invalid = recording;
		auto& event = invalid.transactions[1];
		switch (change)
		{
		case 0:
			invalid.version = 1;
			break;
		case 1:
			event.id = 1;
			break;
		case 2:
			event.commands = {SetGaze{}};
			break;
		case 3:
			event.frame = invalid.originFrame;
			event.afterFrame = false;
			break;
		case 4:
			invalid.originFrame = FrameId{};
			event.frame = FrameId{std::numeric_limits<std::uint64_t>::max()};
			break;
		}
		Rejected(
			[&]
			{
				(void)rigs::EditableRig(invalid);
			},
			"Malformed cleanup event exported");
	}
	recording.transactions[1].afterFrame = false;
	const auto during = rigs::EditableRig(recording);
	Require(during.find("recorded frame +2") == std::string::npos &&
				during.find("kind = \"release_control\"") < during.find("recorded frame +3"),
			"In-boundary sequence cleanup moved to a later boundary");
}
void ScenarioOriginAndSharedCoreReplay()
{
	Instance instance(reinterpret_cast<XrInstance>(1901), {}, XR_MAKE_VERSION(1, 0, 27));
	Session source(reinterpret_cast<XrSession>(1902), instance);
	Session destination(reinterpret_cast<XrSession>(1903), instance);
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	for (auto* session : {&source, &destination})
		Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS,
				"Begin recording origin sessions");
	const CommandSource owner{CommandSourceKind::Rig, 1904};
	const auto submit =
		[&](Session& session, std::uint64_t id, std::uint64_t frame, std::vector<SimulationCommand> commands)
	{
		const CommandTransaction transaction{
			id, owner, {FrameId{frame}, {}}, CommandLatePolicy::Reject, std::move(commands)};
		Require(session.Commands().Submit(transaction).result == CommandResult::Queued, "Queue recording scenario");
	};
	const auto tick = [](Session& session)
	{
		FramePrediction frame;
		Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
					session.EndFrame(frame) == XR_SUCCESS,
				"Publish recording replay boundary");
		return frame;
	};
	submit(source, 1, 1,
		   {TakeControl{ControlChannel::Head}, SetScenario{FailureScenario::HeadUntracked, true},
			TakeControl{ControlChannel::LeftPose}, SetScenario{FailureScenario::LeftDisconnected, true},
			TakeControl{ControlChannel::Session}, SetScenario{FailureScenario::Unfocused, true},
			SetScenario{FailureScenario::Recenter, true}});
	tick(source);
	Require(source.Input().Snapshot().controllers[0].connected == false, "Source scenario did not disconnect");
	source.StartCommandRecording();
	const auto origin = source.Commands().Recorder().Snapshot(0, 0).origin;
	Require(origin.frame.value == 1 && origin.scenarios[static_cast<std::size_t>(FailureScenario::HeadUntracked)] &&
				origin.scenarios[static_cast<std::size_t>(FailureScenario::Recenter)] &&
				origin.configuration.tracking.head.flags == TrackedPoseFlags &&
				origin.configuration.input.controllers[0].connected && origin.configuration.tracking.focused,
			"Recording origin baked scenario effects into recoverable underlying state");
	submit(source, 2, 2,
		   {SetScenario{FailureScenario::HeadUntracked, false}, SetScenario{FailureScenario::LeftDisconnected, false},
			SetScenario{FailureScenario::Unfocused, false}, SetScenario{FailureScenario::Recenter, true}});
	tick(source);
	source.Commands().Recorder().Stop();
	auto recording = rigs::DecodeRecordingPage(Page(source.Commands().Recorder(), 0, 16)).recording;
	Require(recording.transactions.size() == 1, "Initial scenario history was duplicated in transactions");

	submit(destination, 1, 1,
		   {TakeControl{ControlChannel::Body}, SetScenario{FailureScenario::BodyUnavailable, true},
			TakeControl{ControlChannel::Gaze}, SetScenario{FailureScenario::GazeUnavailable, true},
			TakeControl{ControlChannel::Session}, SetScenario{FailureScenario::Hidden, true}});
	tick(destination);
	const auto eventsBefore = instance.Events().Observe();
	const auto lastSequence = eventsBefore.empty() ? 0 : eventsBefore.back().sequence;
	submit(destination, 2, 2, rigs::ReplayInitialCommands(recording));
	const auto restoredFrame = tick(destination);
	const auto restored = destination.Tracking().Sample(restoredFrame.displayTime);
	Require(restored && restored->input.head.flags == ValidPoseFlags && !restored->input.controllers[0].connected &&
				restored->input.body.active && restored->input.gaze.available &&
				destination.Snapshot().state == XR_SESSION_STATE_VISIBLE,
			"Initial replay did not restore persistent failures or clear unrelated failures");
	std::size_t originEvents = 0;
	for (const auto& event : instance.Events().Observe(lastSequence))
		if (std::holds_alternative<ReferenceSpaceEvent>(event.event))
			++originEvents;
	Require(
		originEvents == 2 &&
			!destination.Commands().Snapshot().applied->scenarios[static_cast<std::size_t>(FailureScenario::Recenter)],
		"Restore exact LOCAL/STAGE origins without repeating the consumed recenter trigger");
	const auto recoveryEvents = instance.Events().Observe();
	const auto recoverySequence = recoveryEvents.empty() ? 0 : recoveryEvents.back().sequence;
	submit(destination, 3, 3, recording.transactions.front().commands);
	const auto recoveredFrame = tick(destination);
	const auto recovered = destination.Tracking().Sample(recoveredFrame.displayTime);
	Require(recovered && recovered->input.head.flags == TrackedPoseFlags &&
				destination.Input().Snapshot().controllers[0].connected &&
				destination.Snapshot().state == XR_SESSION_STATE_FOCUSED,
			"Recorded scenario-off commands could not recover the underlying origin");
	std::size_t recenterEvents = 0;
	for (const auto& event : instance.Events().Observe(recoverySequence))
		if (std::holds_alternative<ReferenceSpaceEvent>(event.event))
			++recenterEvents;
	Require(recenterEvents == 2, "A subsequent recorded recenter did not fire exactly once for LOCAL and STAGE");
	auto injected = recording;
	injected.initial.push_back(SetScenario{FailureScenario::Recenter, true});
	Rejected(
		[&]
		{
			(void)rigs::EditableRig(injected);
		},
		"Origin commands injected a consumed recenter trigger");
	recording.initialScenarios[static_cast<std::size_t>(FailureScenario::SessionLoss)] = true;
	Rejected(
		[&]
		{
			(void)rigs::EditableRig(recording);
		},
		"Lost-session origin silently replayed as a live session");
}

void ReferenceSpaceOriginReplay()
{
	Instance instance(reinterpret_cast<XrInstance>(1951), {}, XR_MAKE_VERSION(1, 0, 27));
	Session source(reinterpret_cast<XrSession>(1952), instance);
	Session destination(reinterpret_cast<XrSession>(1953), instance);
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	for (auto* session : {&source, &destination})
		Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS,
				"Begin reference-space recording sessions");
	const CommandSource owner{CommandSourceKind::Rig, 1954};
	const auto submit =
		[&](Session& session, std::uint64_t id, std::uint64_t frame, std::vector<SimulationCommand> commands)
	{
		Require(session.Commands()
						.Submit({id, owner, {FrameId{frame}, {}}, CommandLatePolicy::Reject, std::move(commands)})
						.result == CommandResult::Queued,
				"Queue reference-space boundary");
	};
	const auto tick = [](Session& session)
	{
		FramePrediction frame;
		Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
					session.EndFrame(frame) == XR_SUCCESS,
				"Publish reference-space boundary");
		return frame;
	};
	PoseSample head{{EulerAngles(0.7f, 0, 0), {3, 1.8f, -2}}, TrackedPoseFlags};
	submit(source, 1, 1,
		   {TakeControl{ControlChannel::Head}, SetHeadPose{head}, TakeControl{ControlChannel::Session},
			SetScenario{FailureScenario::Recenter, true}});
	const auto recordedFrame = tick(source);
	source.Spaces().SetStageBounds(std::nullopt);
	source.StartCommandRecording();
	source.Commands().Recorder().Stop();
	const auto recording = rigs::DecodeRecordingPage(Page(source.Commands().Recorder(), 0, 16)).recording;
	const auto expected = source.Spaces().RecordingState(recordedFrame.displayTime);
	Require(!expected.stageBounds && expected.local.position.x == 3 && expected.stage.position.y == 0,
			"Source recenter or unavailable bounds were not captured");
	head.pose = {EulerAngles(-0.5f, 0, 0), {-7, 1.2f, 9}};
	submit(destination, 1, 1,
		   {TakeControl{ControlChannel::Head}, SetHeadPose{head}, TakeControl{ControlChannel::Session},
			SetScenario{FailureScenario::Recenter, true}});
	const auto previousFrame = tick(destination);
	const auto previous = destination.Spaces().RecordingState(previousFrame.displayTime);
	submit(destination, 2, 2, rigs::ReplayInitialCommands(recording));
	const auto replayFrame = tick(destination);
	const auto restored = destination.Spaces().RecordingState(replayFrame.displayTime);
	const auto near = [](float left, float right)
	{
		return std::abs(left - right) < 0.00001f;
	};
	Require(near(restored.local.position.x, expected.local.position.x) &&
				near(restored.local.position.y, expected.local.position.y) &&
				near(restored.local.orientation.y, expected.local.orientation.y) &&
				near(restored.stage.position.z, expected.stage.position.z) && !restored.stageBounds,
			"Replay did not restore the recorded origins and stage-bound availability");
	XrSpace local = XR_NULL_HANDLE;
	Require(destination.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, IdentityPose(), local) ==
				XR_SUCCESS,
			"Create replay reference-space reader");
	PoseSample historical;
	Require(destination.Spaces().WorldPose(local, previousFrame.displayTime, historical) == XR_SUCCESS &&
				near(historical.pose.position.x, previous.local.position.x) &&
				near(historical.pose.orientation.y, previous.local.orientation.y),
			"Origin restoration rewrote reference-space history before its boundary");
	auto invalid = SetReferenceSpaces{restored};
	invalid.value.local.position.x = 99;
	invalid.value.stage.orientation.w = 0;
	invalid.value.stage.orientation.y = 0;
	submit(destination, 3, 3, {invalid});
	const auto rejectedFrame = tick(destination);
	Require(destination.Commands().Snapshot().applied->outcomes.front().result == CommandResult::Invalid &&
				destination.Spaces().RecordingState(rejectedFrame.displayTime).local.position.x ==
					restored.local.position.x,
			"Invalid restoration partially changed an accepted origin");
	destination.ReleaseCommandSource(owner);
	const auto persisted = destination.Spaces().RecordingState(rejectedFrame.displayTime);
	Require(persisted.local.position.x == restored.local.position.x,
			"Releasing command ownership undid a committed origin change");
	auto explicitOrigin = restored;
	explicitOrigin.local.position.x = 10;
	submit(destination, 4, 4,
		   {TakeControl{ControlChannel::Session}, SetReferenceSpaces{explicitOrigin},
			ReleaseControl{ControlChannel::Session}});
	const auto explicitFrame = tick(destination);
	Require(destination.Spaces().RecordingState(explicitFrame.displayTime).local.position.x == 10,
			"Releasing control in the same transaction discarded an origin change");

	head.pose.position.x = 5;
	const auto recenteredHead = head;
	head.pose.position.x = 9;
	submit(destination, 5, 5,
		   {TakeControl{ControlChannel::Session}, TakeControl{ControlChannel::Head}, SetHeadPose{recenteredHead},
			SetScenario{FailureScenario::Recenter, true}, SetHeadPose{head}, ReleaseControl{ControlChannel::Session}});
	const auto orderedFrame = tick(destination);
	const auto ordered = destination.Commands().Snapshot().applied->outcomes.front();
	Require(destination.Spaces().RecordingState(orderedFrame.displayTime).local.position.x == 5 &&
				destination.Tracking().Sample(orderedFrame.displayTime)->head.pose.position.x == 9 &&
				std::get<SetHeadPose>(ordered.transaction.commands[2]).value.pose.position.x == 5,
			"Recenter or acknowledged command values lost transaction order");
	submit(source, 2, 2, ordered.transaction.commands);
	const auto orderedReplay = tick(source);
	Require(source.Spaces().RecordingState(orderedReplay.displayTime).local.position.x == 5,
			"Replaying acknowledged commands changed an intermediate recenter pose");

	explicitOrigin.local.position.x = 12;
	submit(destination, 6, 6, {TakeControl{ControlChannel::Session}, SetReferenceSpaces{explicitOrigin}});
	submit(destination, 7, 6, {ReleaseControl{ControlChannel::Session}});
	const auto releasedFrame = tick(destination);
	Require(destination.Spaces().RecordingState(releasedFrame.displayTime).local.position.x == 12,
			"A later transaction at the same boundary undid an accepted origin change");

	head.flags = 0;
	submit(destination, 8, 7,
		   {TakeControl{ControlChannel::Session}, SetHeadPose{head}, SetScenario{FailureScenario::Recenter, true}});
	const auto unavailableFrame = tick(destination);
	Require(destination.Commands().Snapshot().applied->outcomes.front().result == CommandResult::Invalid &&
				destination.Spaces().RecordingState(unavailableFrame.displayTime).local.position.x == 12 &&
				destination.Tracking().Sample(unavailableFrame.displayTime)->head.pose.position.x == 9,
			"An unavailable recenter pose partially committed tracking or origins");
	auto missing = recording;
	std::erase_if(missing.initial,
				  [](const auto& command)
				  {
					  return std::holds_alternative<SetReferenceSpaces>(command);
				  });
	Rejected(
		[&]
		{
			(void)rigs::EditableRig(missing);
		},
		"Recording without its reference origin was silently replayed");
}

void ReplayFrameLimits()
{
	const FrameId origin{UINT64_MAX - 2};
	rigs::RecordedTransaction release{0, "released", FrameId{UINT64_MAX - 1},
		{ReleaseControl{ControlChannel::Head}}, true};
	Require(rigs::ReplayFrame(release, origin, 4) == FrameId{UINT64_MAX},
		"Release timing lost precision at the final frame");
	release.frame = FrameId{UINT64_MAX};
	Rejected([&] { (void)rigs::ReplayFrame(release, origin, 4); },
		"Relative replay arithmetic hid an overflowing release frame");
	release.phase = "queued";
	release.afterFrame = false;
	Require(!rigs::ReplayFrame(release, origin, 4), "Queued outcomes must not replay");
}

void ExportRequirements()
{
	rigs::InputRecording recording;
	recording.version = 1;
	recording.initial.push_back(SetReferenceSpaces{});
	Require(!rigs::EditableRig(recording).empty(), "Empty stopped recording cannot restore initial state");
	for (int change = 0; change < 5; ++change)
	{
		auto invalid = recording;
		switch (change)
		{
		case 0:
			invalid.active = true;
			break;
		case 1:
			invalid.overflow = true;
			break;
		case 2:
			invalid.total = 1;
			break;
		case 3:
			invalid.version = 5;
			break;
		case 4:
			invalid.total = 1;
			invalid.transactions.push_back({1, "applied", FrameId{0}, {SetGaze{}}});
			break;
		}
		Rejected(
			[&]
			{
				(void)rigs::EditableRig(invalid);
			},
			"Incomplete or invalid recording exported");
	}
}
void TrackerCollectionRecording()
{
	CommandRecording source;
	source.origin.configuration.tracking.trackers.clear();
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
		source.origin.configuration.tracking.trackers.push_back({{100 + index}, TrackerRole::Unassigned});
	source.origin.configuration.tracking.trackers.back().sourcePath = TrackerSourcePath{"htcx:/device/physical_163"};
	wire::Message page;
	page.fields.push_back(wire::Field::Bytes(wire::tag::Document, wire::SimulationRecording(source).Encode()));
	auto restored = rigs::DecodeRecordingPage(page).recording;
	Require(restored.version == 3, "Tracker recording did not use schema 3");
	const auto initial = rigs::ReplayInitialCommands(restored);
	CommandState state;
	CommandTransaction transaction;
	transaction.source = {CommandSourceKind::Rig, 1};
	transaction.commands = initial;
	Require(state.Apply(transaction, {}, {Duration{1}}) == CommandResult::Applied &&
				state.Merge({}).tracking.trackers.size() == MaxTrackerDevices &&
				state.Merge({}).tracking.trackers.back().identity.value == 163 &&
				state.Merge({}).tracking.trackers.back().sourcePath.View() == "htcx:/device/physical_163",
			"Recorded collection origin did not restore the complete inventory");
	Require(rigs::EditableRig(restored).find("htcx:/device/physical_163") != std::string::npos &&
			 rigs::EditableRig(restored).find("identity = \"163\"") != std::string::npos,
			"Editable rig omitted the last tracker");
	for (std::uint32_t version : {1u, 2u})
	{
		restored.version = version;
		Rejected(
			[&]
			{
				(void)rigs::EditableRig(restored);
			},
			"Legacy schema accepted a collection command");
		CommandRecording legacy;
		legacy.version = version;
		page.fields.front().value = wire::SimulationRecording(legacy).Encode();
		const auto old = rigs::DecodeRecordingPage(page).recording;
		Require(old.version == version && !rigs::EditableRig(old).empty(), "Legacy recording stopped exporting");
	}
}
void TrackerCollectionCodec()
{
	SetTrackers collection;
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
		collection.value.push_back({{100 + index}, TrackerRole::Unassigned});
	collection.value.back().poseFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	collection.value.back().sourcePath = TrackerSourcePath{"htcx:/device/physical_163"};
	const auto encoded = wire::EncodeSimulationCommand(collection);
	const auto decoded = std::get<SetTrackers>(wire::DecodeSimulationCommand(encoded));
	Require(decoded.value.size() == MaxTrackerDevices && decoded.value.back().identity.value == 163 &&
				decoded.value.back().sourcePath.View() == "htcx:/device/physical_163" &&
				decoded.value.back().poseFlags == XR_SPACE_LOCATION_ORIENTATION_VALID_BIT,
			"Tracker collection codec truncated identities");
	Require(wire::EncodeSimulationCommand(decoded).value == encoded.value, "Tracker collection codec changed fields");
	Require(std::get<SetTrackers>(wire::DecodeSimulationCommand(wire::EncodeSimulationCommand(SetTrackers{})))
				.value.empty(),
			"Empty collection gained default devices");
	collection.value.push_back({{999}, TrackerRole::Unassigned});
	Rejected(
		[&]
		{
			(void)wire::EncodeSimulationCommand(collection);
		},
		"Oversized collection encoded");
	auto corrupt = encoded;
	auto envelope = wire::DecodeFields(corrupt.value).fields;
	auto entries = wire::DecodeFields(envelope[1].value).fields;
	entries[1].tag = 65;
	envelope[1].value = wire::EncodeFields(entries);
	corrupt.value = wire::EncodeFields(envelope);
	Rejected(
		[&]
		{
			(void)wire::DecodeSimulationCommand(corrupt);
		},
		"Sparse tracker collection decoded");
}
void RejectLossyCommandDecoding()
{
	SetReferenceSpaces spaces;
	spaces.value.stageBounds.reset();
	auto encodedSpaces = wire::EncodeSimulationCommand(spaces);
	auto spaceFields = wire::DecodeFields(encodedSpaces.value).fields;
	const auto available = wire::EncodeSimulationCommand(SetReferenceSpaces{});
	const auto availableFields = wire::DecodeFields(available.value).fields;
	spaceFields[1] = availableFields[1];
	spaceFields[1].value = wire::EncodeFields(
		[&]
		{
			auto payload = wire::DecodeFields(spaceFields[1].value).fields;
			payload[2] = wire::Field::Bool(3, false, true);
			return payload;
		}());
	encodedSpaces.value = wire::EncodeFields(spaceFields);
	Rejected(
		[&]
		{
			(void)wire::DecodeSimulationCommand(encodedSpaces);
		},
		"Unavailable bounds silently discarded nonzero dimensions");
	auto encoded = wire::EncodeSimulationCommand(SetGaze{});
	auto fields = wire::DecodeFields(encoded.value).fields;
	fields.push_back(wire::Field::U32(65000, 1));
	encoded.value = wire::EncodeFields(fields);
	std::string hex;
	constexpr std::string_view digits = "0123456789abcdef";
	for (const auto byte : encoded.value)
	{
		const auto value = std::to_integer<unsigned>(byte);
		hex += digits[value >> 4];
		hex += digits[value & 15];
	}
	using Value = wire::InspectionValue;
	auto document = Value::Object(
		{{"version", Value::Unsigned(1)},
		 {"seed", Value::Unsigned(0)},
		 {"total", Value::Unsigned(0)},
		 {"origin_frame", Value::Unsigned(0)},
		 {"origin_time", Value::Signed(0)},
		 {"active", Value::Boolean(false)},
		 {"overflow", Value::Boolean(false)},
		 {"initial_scenarios", Value::Array({})},
		 {"initial_commands", Value::Array({Value::Object(
								  {{"encoding", Value::Text("simxr-command-v1-hex")}, {"data", Value::Text(hex)}})})},
		 {"transactions", Value::Array({})}});
	wire::Message response;
	response.fields.push_back(wire::Field::Bytes(wire::tag::Document, document.Encode()));
	Rejected(
		[&]
		{
			(void)rigs::DecodeRecordingPage(response);
		},
		"An unknown optional command field was silently omitted from editable export");
}
} // namespace
int main()
{
	try
	{
		BodyRecordingSchema();
		CollectionIdentityAndRollback();
		ReleaseEventExport();
		ScenarioOriginAndSharedCoreReplay();
		ReferenceSpaceOriginReplay();
		ReplayFrameLimits();
		ExportRequirements();
		TrackerCollectionRecording();
		TrackerCollectionCodec();
		RejectLossyCommandDecoding();
		std::cout << "PASS recording pages, transactional collection and editable export bounds\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
