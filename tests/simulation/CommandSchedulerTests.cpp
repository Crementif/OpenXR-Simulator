#include "../TestSupport.h"
#include "math/Math.h"
#include "runtime/Session.h"
#include "simulation/CommandScheduler.h"
#include "simulation/CommandState.h"

#include <cmath>
#include <cstdlib>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <utility>

namespace
{
thread_local bool failCommandAllocation = false;
thread_local int commandAllocationCountdown = -1;
} // namespace
void* operator new(std::size_t size)
{
	if (std::exchange(failCommandAllocation, false) ||
		(commandAllocationCountdown >= 0 && commandAllocationCountdown-- == 0))
		throw std::bad_alloc{};
	if (void* memory = std::malloc(size ? size : 1))
		return memory;
	throw std::bad_alloc{};
}
void* operator new(std::size_t size, const std::nothrow_t&) noexcept
{
	try
	{
		return ::operator new(size);
	}
	catch (const std::bad_alloc&)
	{
		return nullptr;
	}
}
void operator delete(void* memory, const std::nothrow_t&) noexcept
{
	::operator delete(memory);
}
[[gnu::noinline]] void operator delete(void* memory) noexcept
{
	std::free(memory);
}
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept
{
	std::free(memory);
}

using namespace simxr;
namespace
{
const CommandSource Manual{CommandSourceKind::Manual, 101};
const CommandSource Mcp{CommandSourceKind::Mcp, 202};
CommandTransaction Request(std::uint64_t id, std::uint64_t frame, std::vector<SimulationCommand> commands,
						   CommandSource source = Manual)
{
	return {id, source, {FrameId{frame}, {}}, CommandLatePolicy::Reject, std::move(commands)};
}
std::shared_ptr<const AppliedCommandSnapshot> Tick(CommandScheduler& scheduler, std::uint64_t frame,
												   const CommandConfiguration& baseline = {})
{
	return scheduler.Apply({frame}, {Duration{static_cast<std::int64_t>(frame * 10)}},
						   {Duration{static_cast<std::int64_t>(frame * 10 + 100)}}, baseline);
}
PoseSample Pose(float x)
{
	return {{{0, 0, 0, 1.005f}, {x, 1.8f, -0.5f}}, TrackedPoseFlags};
}
std::shared_ptr<const PoseSequence> SequenceProgram(bool loop = false)
{
	PoseSequenceStep first, second;
	first.head = HeadPosePatch{};
	first.head->position[0] = 4;
	first.controllers[1] = ControllerPosePatch{};
	first.controllers[1]->trigger = .7f;
	first.capture = true;
	second.frame = {1};
	second.head = HeadPosePatch{};
	second.head->angles[1] = .25f;
	return std::make_shared<const PoseSequence>(std::vector{first, second}, loop);
}
CommandConfiguration SequenceBaseline()
{
	CommandConfiguration baseline;
	baseline.tracking.head = {{EulerAngles(.4f, -.2f, .3f), {1, 2, 3}}, TrackedPoseFlags};
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		baseline.tracking.controllers[hand].pose = {
			Compose(baseline.tracking.head.pose, {{0, 0, 0, 1}, {hand ? .2f : -.2f, -.3f, -.4f}}), TrackedPoseFlags};
		baseline.input.controllers[hand].grip = .2f;
	}
	return baseline;
}
void ScheduledPoseSequence()
{
	CommandScheduler scheduler;
	scheduler.Start();
	const auto baseline = SequenceBaseline();
	scheduler.StartRecording(baseline, {});
	const auto admitted = scheduler.StartPoseSequence(Mcp, SequenceProgram());
	Require(admitted && admitted->active && admitted->result == CommandResult::Queued &&
				!scheduler.StartPoseSequence(Manual, SequenceProgram()),
			"Sequence admission did not preserve exclusive ownership");
	const auto first = Tick(scheduler, 1, baseline);
	Require(first->configuration.tracking.head.pose.position.x == 4 &&
				first->configuration.input.controllers[1].trigger == .7f &&
				first->configuration.input.controllers[1].grip == .2f && first->sequenceCapture &&
				first->sequenceCapture->frame == FrameId{1} && first->sequenceCapture->sequence == admitted->id,
			"Sequence changed sparse fields or lost its exact capture intent");
	const auto identity = CommandSource{CommandSourceKind::Sequence, admitted->id};
	Require(first->owners[0] == identity && first->outcomes.size() == 1 &&
				first->outcomes[0].transaction.source == identity,
			"Sequence bypassed shared command ownership/history");
	scheduler.Submit(Request(1, 2, {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(99)}}));
	const auto second = Tick(scheduler, 2, baseline);
	const auto expectedHead = XrPosef{EulerAngles(.4f, .25f, .3f), {4, 2, 3}};
	const auto expectedHand = Compose(expectedHead, {{0, 0, 0, 1}, {.2f, -.3f, -.4f}});
	Require(second->outcomes[0].result == CommandResult::Conflict && !second->sequenceCapture &&
				std::abs(second->configuration.tracking.head.pose.orientation.y - expectedHead.orientation.y) <
					.00001f &&
				std::abs(second->configuration.tracking.controllers[1].pose.pose.position.z - expectedHand.position.z) <
					.00001f,
			"Sequence lost partial angles, head-local controller motion or ownership arbitration");
	const auto completed = scheduler.PoseSequenceStatusFor(Mcp);
	Require(completed && !completed->active && completed->groups == 2 && completed->origin == FrameId{1} &&
				completed->lastFrame == FrameId{2} && completed->result == CommandResult::Applied,
			"Sequence completion lacks exact applied frame evidence");
	const auto recording = scheduler.Recorder().Snapshot();
	Require(recording.transactions.size() == 3 && recording.transactions[2].transaction.source == identity,
			"Expanded sequence commands did not enter the existing recorder");
	scheduler.ReleaseSource(Mcp, true);
	const auto released = Tick(scheduler, 3, baseline);
	Require(!released->owners[0] && released->configuration.tracking.head.pose.position.x == 1 &&
				scheduler.PoseSequenceStatusFor(Mcp)->result == CommandResult::Cancelled,
			"Connection retirement left sequence input ownership alive");
}
void PoseSequenceReservationRollback()
{
	CommandScheduler scheduler;
	scheduler.Start();
	const auto baseline = SequenceBaseline();
	scheduler.StartRecording(baseline, {});
	const auto admitted = scheduler.StartPoseSequence(Mcp, SequenceProgram(true));
	bool rejected = false;
	try
	{
		scheduler.Apply({1}, {Duration{10}}, {Duration{110}}, baseline,
						[](const auto& prepared)
						{
							Require(prepared->sequenceCapture.has_value(),
									"Sequence was absent from the prospective boundary");
							throw std::bad_alloc{};
						});
	}
	catch (const std::bad_alloc&)
	{
		rejected = true;
	}
	Require(rejected && scheduler.PoseSequenceStatusFor(Mcp)->origin == FrameId{} &&
				scheduler.PoseSequenceStatusFor(Mcp)->groups == 0 &&
				scheduler.Recorder().Snapshot().transactions.empty() && !scheduler.Snapshot().applied,
			"Failed reservation advanced sequence, ownership or recording");
	Require(Tick(scheduler, 1, baseline)->sequenceCapture.has_value(), "Retry lost first sequence capture intent");
	Tick(scheduler, 2, baseline);
	Require(Tick(scheduler, 3, baseline)->sequenceCapture.has_value() &&
				scheduler.PoseSequenceStatusFor(Mcp)->groups == 3,
			"Loop boundary lost a step or capture intent");
	Require(Tick(scheduler, 3, baseline)->sequenceCapture.has_value() &&
				scheduler.PoseSequenceStatusFor(Mcp)->groups == 3,
			"Repeated frame advanced playback twice");
	PoseSequenceStep impossible;
	impossible.frame = {UINT64_MAX};
	rejected = false;
	try
	{
		scheduler.StartPoseSequence(Mcp, std::make_shared<const PoseSequence>(std::vector{impossible}, false));
	}
	catch (const std::overflow_error&)
	{
		rejected = true;
	}
	Require(rejected && scheduler.PoseSequenceStatusFor(Mcp)->id == admitted->id &&
				scheduler.PoseSequenceStatusFor(Mcp)->active,
			"Invalid replacement abandoned active sequence");
	PoseSequenceStep sparse;
	sparse.controllers[1] = ControllerPosePatch{};
	sparse.controllers[1]->grip = .8f;
	sparse.capture = true;
	const auto replacement =
		scheduler.StartPoseSequence(Mcp, std::make_shared<const PoseSequence>(std::vector{sparse}, false));
	Require(replacement && replacement->id != admitted->id, "Replacement reused execution identity");
	const auto replaced = Tick(scheduler, 4, baseline);
	Require(replaced->sequenceCapture->sequence == replacement->id &&
				replaced->owners[0] == CommandSource{CommandSourceKind::Sequence, replacement->id},
			"Replacement conflicted with its predecessor or lost capture identity");
	Require(replaced->configuration.tracking.head.pose.position.x == 4 &&
				replaced->configuration.input.controllers[1].trigger == .7f &&
				replaced->configuration.input.controllers[1].grip == .8f,
			"Sparse replacement dropped values established by the preceding sequence");
	scheduler.StopPoseSequence(Mcp);
	Require(!Tick(scheduler, 5, baseline)->owners[0], "Stopped sequence kept its input channels");
}
void PoseSequenceFailureAndCaptureOnly()
{
	CommandScheduler scheduler;
	scheduler.Start();
	const auto baseline = SequenceBaseline();
	scheduler.Submit(Request(1, 1, {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(9)}}));
	scheduler.StartPoseSequence(Mcp, SequenceProgram());
	const auto conflict = Tick(scheduler, 1, baseline);
	Require(scheduler.PoseSequenceStatusFor(Mcp)->result == CommandResult::Conflict &&
				conflict->configuration.tracking.head.pose.position.x == 9 && !conflict->sequenceCapture &&
				!conflict->owners[1],
			"Failed sequence partially acquired input or emitted capture intent");
	scheduler.ReleaseSource(Manual);
	scheduler.StartPoseSequence(Mcp, SequenceProgram(true));
	Tick(scheduler, 2, baseline);
	const auto late = Tick(scheduler, 4, baseline);
	Require(scheduler.PoseSequenceStatusFor(Mcp)->result == CommandResult::Late && !late->sequenceCapture &&
				!late->owners[0],
			"Missed sequence frame was silently retimed");
	PoseSequenceStep capture;
	capture.capture = true;
	scheduler.StartPoseSequence(Mcp, std::make_shared<const PoseSequence>(std::vector{capture}, false));
	const auto captured = Tick(scheduler, 5, baseline);
	Require(captured->sequenceCapture && captured->outcomes.empty() && !captured->owners[0] &&
				!scheduler.PoseSequenceStatusFor(Mcp)->active,
			"Capture-only sequence took unrelated input ownership");
	scheduler.Close();
	Require(!scheduler.StartPoseSequence(Mcp, SequenceProgram()), "Closed scheduler accepted a sequence");
	scheduler.Start();
	Require(!scheduler.PoseSequenceStatusFor(Mcp), "Session restart inherited an old sequence");
}

void SequenceLocalControllerFollowing()
{
	CommandScheduler scheduler;
	scheduler.Start();
	auto baseline = SequenceBaseline();
	PoseSequenceStep step;
	step.controllers[1] = ControllerPosePatch{};
	step.controllers[1]->position[0] = .8f;
	step.controllers[1]->grip = .6f;
	step.controllers[1]->primary = true;
	const auto admitted =
		scheduler.StartPoseSequence(Mcp, std::make_shared<const PoseSequence>(std::vector{step}, false));
	Tick(scheduler, 1, baseline);
	baseline.tracking.head.pose = {EulerAngles(.7f, .2f, 0), {6, 3, 4}};
	const auto moved = Tick(scheduler, 2, baseline);
	const auto expected = Compose(baseline.tracking.head.pose, {{0, 0, 0, 1}, {.8f, -.3f, -.4f}});
	Require(!scheduler.PoseSequenceStatusFor(Mcp)->active && !moved->owners[0] &&
				std::abs(moved->configuration.tracking.controllers[1].pose.pose.position.x - expected.position.x) <
					.00001f &&
				std::abs(moved->configuration.tracking.controllers[1].pose.pose.position.z - expected.position.z) <
					.00001f,
			"Completed local controller patch stopped following the independently controlled head");
	const auto& controls = moved->configuration.input.controllers[1];
	Require(controls.grip == .6f && controls.gripForce == .6f && controls.primary && controls.primaryTouch,
			"Legacy sequence controls lost their derived action fields");
	Require(
		scheduler
				.Submit(Request(1, 3, {TakeControl{ControlChannel::Head}}, {CommandSourceKind::Sequence, admitted->id}))
				.result == CommandResult::Invalid,
		"External commands impersonated a runtime sequence");
	Require(scheduler
					.Submit(Request(1, 3, {TakeControl{ControlChannel::RightPose, true}},
									{CommandSourceKind::Manual, admitted->id}))
					.result == CommandResult::Queued,
			"Sequence history consumed a caller transaction identity");
	const auto takeover = Tick(scheduler, 3, baseline);
	Require(scheduler.PoseSequenceStatusFor(Mcp)->result == CommandResult::Conflict &&
				takeover->owners[2] == CommandSource{CommandSourceKind::Manual, admitted->id} && !takeover->owners[4],
			"Explicit takeover left the sequence's other channels held");
}

void SequenceTransfersCallerChannels()
{
	CommandScheduler commands;
	commands.Start();
	commands.Submit(Request(1, 1, {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(9)}}, Mcp));
	Tick(commands, 1, SequenceBaseline());
	const auto sequence = commands.StartPoseSequence(Mcp, SequenceProgram());
	const auto applied = Tick(commands, 2, SequenceBaseline());
	Require(applied->configuration.tracking.head.pose.position.x == 4 &&
				applied->owners[0] == CommandSource{CommandSourceKind::Sequence, sequence->id} &&
				applied->outcomes[0].result == CommandResult::Applied &&
				std::get<TakeControl>(applied->outcomes[0].transaction.commands[0]).replace,
			"Sequence conflicted with its caller's earlier one-shot control instead of recording ownership transfer");
}

void SequenceCaptureIssueRollback()
{
	FrameHistory history;
	const auto generation = history.StartGeneration();
	FramePool pool(1);
	SessionCaptureCoordinator captures(history);
	Require(captures.Configure(FramePool(pool), false, {}, false, false) == XR_SUCCESS,
			"Configure sequence capture rollback");
	captures.Start(generation);
	CommandScheduler commands;
	commands.Start();
	commands.StartPoseSequence(Mcp, SequenceProgram());
	const FramePrediction prediction{{1}, {1}, {Duration{110}}, Duration{10}};
	bool rejected = false;
	try
	{
		commands.Apply(prediction.id, {Duration{10}}, prediction.displayTime, SequenceBaseline(),
					   [&](const auto& applied)
					   {
						   auto capture = captures.PrepareSequenceCapture(applied->sequenceCapture);
						   Require(capture.Record()->reservation.result == SnapshotAdmission::Accepted &&
									   pool.InUse() == 1,
								   "Sequence did not reserve before history admission");
						   failCommandAllocation = true;
						   history.Issue(prediction, {}, capture.Record());
					   });
	}
	catch (const std::bad_alloc&)
	{
		rejected = true;
	}
	Require(rejected && pool.InUse() == 0 && captures.Snapshots().Snapshot().captures.empty() &&
				!commands.At(prediction.id) && !commands.PoseSequenceStatusFor(Mcp)->origin.value &&
				!history.Find({generation, prediction.id}),
			"Failed issue leaked a ticket or advanced sequence/history");
	commands.Apply(prediction.id, {Duration{10}}, prediction.displayTime, SequenceBaseline(),
				   [&](const auto& applied)
				   {
					   auto capture = captures.PrepareSequenceCapture(applied->sequenceCapture);
					   history.Issue(
						   prediction,
						   [&](auto issued, auto cancelled)
						   {
							   capture.Bind(std::move(issued), std::move(cancelled));
						   },
						   capture.Record());
				   });
	const auto issued = history.Find({generation, prediction.id});
	Require(issued->sequenceCapture && issued->sequenceCapture->request.owner == Mcp &&
				captures.Snapshots().Read(issued->sequenceCapture->reservation.id)->frame == issued->key &&
				pool.InUse() == 1,
			"Retry failed to bind the exact sequence frame");
}

void AllocationFreeSpaceCommit()
{
	TrackingSystem tracking;
	EventQueue events(2);
	SpaceSystem spaces(reinterpret_cast<XrSession>(701), tracking, {}, {}, &events);
	ReferenceSpaceState state;
	bool accepted = true;
	failCommandAllocation = true;
	for (std::int64_t boundary = 1; boundary <= 140; ++boundary)
	{
		state.local.position.x = static_cast<float>(boundary);
		accepted = accepted && spaces.Restore({Duration{boundary}}, state) == XR_SUCCESS;
	}
	const bool noAllocation = failCommandAllocation;
	failCommandAllocation = false;
	Require(accepted && noAllocation && spaces.RecordingState({Duration{140}}).local.position.x == 140 &&
				events.Observe().size() == 2,
			"Accepted origin/history/event publication must remain allocation free at full retention");
}

void AtomicCommandState()
{
	CommandState state;
	CommandConfiguration baseline;
	baseline.tracking.head.pose.position.x = -9;
	baseline.tracking.gaze.yaw = 0.03f;
	const RuntimeTime time{Duration{101}};
	auto initial = Request(1, 1, {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(2)}});
	Require(state.Apply(initial, baseline, time) == CommandResult::Applied &&
				std::abs(std::get<SetHeadPose>(initial.commands[1]).value.pose.orientation.w - 1) < 0.00001f,
			"Command state acknowledges normalized values");
	auto conflict = Request(1, 1,
							{TakeControl{ControlChannel::Gaze}, SetGaze{{0.8f, 0, true, true}},
							 TakeControl{ControlChannel::Head}, SetHeadPose{Pose(8)}},
							Mcp);
	Require(state.Apply(conflict, baseline, time) == CommandResult::Conflict,
			"A transaction cannot steal an owned channel");
	AppliedCommandSnapshot snapshot;
	state.Capture(snapshot, baseline, time);
	Require(snapshot.configuration.tracking.head.pose.position.x == 2 &&
				snapshot.configuration.tracking.gaze.yaw == baseline.tracking.gaze.yaw &&
				!snapshot.owners[static_cast<std::size_t>(ControlChannel::Gaze)],
			"A later conflict leaked earlier channel ownership or values");
	auto invalid = Request(2, 1,
						   {SetHeadPose{Pose(7)}, TakeControl{ControlChannel::Gaze},
							SetGaze{{std::numeric_limits<float>::quiet_NaN(), 0, true, true}}});
	Require(state.Apply(invalid, baseline, time) == CommandResult::Invalid &&
				state.Merge(baseline).tracking.head.pose.position.x == 2,
			"Invalid state transaction partially applied an earlier pose");

	ControllerControls controls;
	controls.trigger = 0.7f;
	auto replace = Request(2, 1,
						   {TakeControl{ControlChannel::Head, true}, SetHeadPose{Pose(8)},
							TakeControl{ControlChannel::LeftControls}, SetControllerControls{0, controls}},
						   Mcp);
	Require(state.Apply(replace, baseline, time) == CommandResult::Applied, "Explicit channel replacement");
	failCommandAllocation = true;
	const auto released = state.ReleaseSource(Mcp);
	const bool didNotAllocate = failCommandAllocation;
	failCommandAllocation = false;
	Require(didNotAllocate && released[static_cast<std::size_t>(ControlChannel::Head)] &&
				released[static_cast<std::size_t>(ControlChannel::LeftControls)],
			"Releasing ownership must remain allocation free");
	baseline.input.controllers[0].trigger = 0.9f;
	baseline.input.controllers[0].connected = false;
	baseline.tracking.controllers[0].connected = false;
	const auto releasedState = state.Merge(baseline);
	Require(releasedState.tracking.head.pose.position.x == -9 && releasedState.input.controllers[0].trigger == 0 &&
				!releasedState.input.controllers[0].connected,
			"Release must restore baseline pose, neutralize held controls and preserve disconnection");
	state.ClearOwnership();
	Require(state.Merge(baseline).input.controllers[0].trigger == 0.9f,
			"Closed command state retained a neutral override");
}

void PreparedBoundaryAllocationRollback()
{
	CommandScheduler scheduler;
	scheduler.Start(41);
	scheduler.Submit(Request(1, 1, {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(2)}}));
	const auto original = Tick(scheduler, 1);
	scheduler.Recorder().Start({41, original->configuration});
	scheduler.Submit(Request(2, 2, {SetHeadPose{Pose(8)}}));
	bool failed = false;
	bool reserved = false;
	failCommandAllocation = true;
	try
	{
		scheduler.Apply({2}, {Duration{20}}, {Duration{120}}, {},
						[&](const auto&)
						{
							reserved = true;
						});
	}
	catch (const std::bad_alloc&)
	{
		failed = true;
	}
	failCommandAllocation = false;
	Require(failed && !reserved && scheduler.At({1}) == original && !scheduler.At({2}) &&
				scheduler.Snapshot().pending == 1 && scheduler.Recorder().Snapshot().total == 0 &&
				scheduler.Merge({}).tracking.head.pose.position.x == 2,
			"Failed boundary preparation changed a published state, queue, recorder or external reservation");
	const auto retried = Tick(scheduler, 2);
	Require(retried->revision == original->revision + 1 && retried->configuration.tracking.head.pose.position.x == 8 &&
				scheduler.Recorder().Snapshot().total == 1 &&
				original->configuration.tracking.head.pose.position.x == 2,
			"Boundary retry must publish once and preserve the earlier immutable reader");
}

void ImmutableRecordingPublication()
{
	CommandScheduler scheduler;
	scheduler.Start(17);
	scheduler.Recorder().Start({17});
	std::vector<CommandOutcome> priorOutcomes(1024);
	Require(scheduler.Recorder().Observe(priorOutcomes), "Fill the bounded recorder exactly");
	scheduler.Submit(Request(1, 1, {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(8)}}));
	std::shared_ptr<const AppliedCommandSnapshot> rejected;
	bool failed = false;
	try
	{
		scheduler.Apply({1}, {Duration{10}}, {Duration{110}}, {},
						[&](const auto& snapshot)
						{
							rejected = snapshot;
							const auto recording = scheduler.Recorder().Snapshot(0, 0);
							Require(
								recording.active && !recording.overflow && recording.total == 1024 &&
									snapshot->outcomes.front().recordingOverrun &&
									std::abs(std::get<SetHeadPose>(snapshot->outcomes.front().transaction.commands[1])
												 .value.pose.orientation.w -
											 1) < 0.00001f,
								"Reservation must see finalized snapshot flags and may inspect the prior recorder");
							throw std::bad_alloc{};
						});
	}
	catch (const std::bad_alloc&)
	{
		failed = true;
	}
	const auto recording = scheduler.Recorder().Snapshot(0, 0);
	Require(failed && rejected && recording.active && !recording.overflow && recording.total == 1024 &&
				!scheduler.At({1}) && scheduler.Snapshot().pending == 1,
			"Rejected external reservation published command or recorder overflow state");
	const auto rejectedOutcomes = rejected->outcomes;
	std::shared_ptr<const AppliedCommandSnapshot> accepted;
	const auto applied = scheduler.Apply(
		{1}, {Duration{10}}, {Duration{110}}, {},
		[&](const auto& snapshot)
		{
			accepted = snapshot;
			Require(snapshot->outcomes.front().recordingOverrun && scheduler.Recorder().Snapshot(0, 0).active,
					"Retry exposed an unfinished recording flag or locked out read-only recorder inspection");
		});
	const auto committedRecording = scheduler.Recorder().Snapshot(0, 0);
	Require(applied == accepted && applied != rejected && applied->revision == 1 &&
				scheduler.Read(Manual, 1)->recordingOverrun && applied->outcomes.front().recordingOverrun &&
				rejected->outcomes.front().recordingOverrun &&
				rejected->outcomes.front().transaction.id == rejectedOutcomes.front().transaction.id &&
				!committedRecording.active && committedRecording.overflow && committedRecording.total == 1024,
			"Retained prepared snapshots changed after reservation or recorder overflow was hidden");
}

void LateAdmissionRollback()
{
	for (int failure = 0; failure < 256; ++failure)
	{
		CommandScheduler scheduler;
		scheduler.Start(73);
		Tick(scheduler, 1);
		scheduler.Recorder().Start({73});
		Require(scheduler.Submit(Request(1, 1, {TakeControl{ControlChannel::Head}}, Mcp)).result == CommandResult::Late,
				"Prepare existing late-command history");
		auto request = Request(5, 1, {TakeControl{ControlChannel::Gaze}});
		bool failed = false;
		commandAllocationCountdown = failure;
		try
		{
			(void)scheduler.Submit(std::move(request));
		}
		catch (const std::bad_alloc&)
		{
			failed = true;
		}
		commandAllocationCountdown = -1;
		if (failed)
		{
			const auto history = scheduler.Snapshot();
			const auto recording = scheduler.Recorder().Snapshot();
			Require(!scheduler.Read(Manual, 5) && history.pending == 0 && history.outcomes.size() == 1 &&
						history.outcomes.front().transaction.source == Mcp && recording.active && !recording.overflow &&
						recording.total == 1,
					"Failed late admission published history or changed recording");
			Require(scheduler.Submit(Request(5, 1, {TakeControl{ControlChannel::Gaze}})).result == CommandResult::Late,
					"Retry late admission");
		}
		Require(scheduler.Read(Manual, 5)->result == CommandResult::Late && scheduler.Snapshot().outcomes.size() == 2 &&
					scheduler.Recorder().Snapshot().total == 2 &&
					scheduler.Submit(Request(4, 2, {TakeControl{ControlChannel::Gaze}})).result ==
						CommandResult::Conflict,
				"Late admission retry duplicated recording or failed to commit its source cursor");
		if (!failed)
		{
			std::cout << "Late admission allocation sweep: " << failure << " failures\n";
			return;
		}
	}
	throw std::runtime_error("Late admission allocation sweep exceeded its bound");
}

void SourceReleaseRecording()
{
	CommandScheduler scheduler;
	scheduler.Start();
	scheduler.StartRecording({}, {});
	ControllerControls held;
	held.trigger = .75f;
	scheduler.Submit(Request(1, 1, {TakeControl{ControlChannel::LeftControls}, SetControllerControls{0, held}}));
	Tick(scheduler, 1);
	scheduler.ReleaseSource(Manual);
	scheduler.Submit(Request(1, 3, {TakeControl{ControlChannel::Gaze}, SetGaze{{.2f}}}, Mcp));
	Require(Tick(scheduler, 2)->configuration.input.controllers[0].trigger == 0, "Live cleanup did not release input");
	Tick(scheduler, 3);
	scheduler.Recorder().Stop();
	const auto recording = scheduler.Recorder().Snapshot();
	Require(recording.total == 3 && !recording.overflow, "Recording omitted intermediate source release");
	const auto& release = recording.transactions[1];
	Require(release.result == CommandResult::Released && release.transaction.id == 0 &&
				release.transaction.source == Manual && release.afterFrame && release.appliedFrame == FrameId{1} &&
				release.appliedTime == SimulationTime{Duration{10}} && release.transaction.commands.size() == 1 &&
				std::get<ReleaseControl>(release.transaction.commands[0]).channel == ControlChannel::LeftControls,
			"Release lost its source, channel or position after the committed boundary");
	CommandScheduler replay;
	replay.Start(recording.origin.seed, recording.origin.configuration);
	std::uint64_t id = 0;
	for (const auto& outcome : recording.transactions)
		Require(
			replay.Submit(Request(++id, outcome.appliedFrame.value + outcome.afterFrame, outcome.transaction.commands))
					.result == CommandResult::Queued,
			"Replay admission failed");
	Require(Tick(replay, 1)->configuration.input.controllers[0].trigger == .75f &&
				Tick(replay, 2)->configuration.input.controllers[0].trigger == 0 &&
				Tick(replay, 3)->configuration.tracking.gaze.yaw == .2f,
			"Replay moved cleanup to the held frame or after the later command");
	Require(scheduler.Read(Manual, 1)->result == CommandResult::Applied &&
				scheduler.Submit(Request(2, 4, {TakeControl{ControlChannel::LeftControls}})).result ==
					CommandResult::Queued,
			"Internal release event changed client retry history or cursor");
}
void SourceReleaseRecordingFailure()
{
	int failures = 0;
	for (int allocation = 0; allocation < 256; ++allocation)
	{
		CommandScheduler scheduler;
		scheduler.StartRecording({}, {});
		ControllerControls controls;
		controls.trigger = .75f;
		scheduler.Submit(
			Request(1, 1, {TakeControl{ControlChannel::LeftControls}, SetControllerControls{0, controls}}));
		Tick(scheduler, 1);
		commandAllocationCountdown = allocation;
		scheduler.ReleaseSource(Manual);
		commandAllocationCountdown = -1;
		const auto recording = scheduler.Recorder().Snapshot();
		Require(scheduler.Merge({}).input.controllers[0].trigger == 0, "Recording failure prevented live release");
		if (!recording.overflow)
		{
			Require(recording.total == 2 && recording.transactions.back().result == CommandResult::Released,
					"Complete recording omitted release");
			scheduler.ReleaseSource(Manual);
			Require(scheduler.Recorder().Snapshot().total == 2, "No-op release duplicated an event");
			Require(failures > 0, "Release allocation sweep missed fallible recording work");
			std::cout << "Source release allocation sweep: " << failures << " failures\n";
			return;
		}
		++failures;
		Require(!recording.active, "Failed cleanup recording still claims completeness");
	}
	throw std::runtime_error("Source release allocation sweep did not complete");
}
void RecordedReleaseState()
{
	for (const bool reject : {false, true})
	{
		CommandScheduler scheduler;
		CommandConfiguration baseline;
		baseline.tracking.head = Pose(9);
		baseline.input.controllers[0].trigger = .2f;
		scheduler.StartRecording(baseline, {});
		ControllerControls controls;
		controls.trigger = .75f;
		scheduler.Submit(Request(1, 1,
								 {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(4)},
								  TakeControl{ControlChannel::LeftControls}, SetControllerControls{0, controls}}));
		scheduler.Submit(Request(
			1, 1, {TakeControl{ControlChannel::RightPose}, SetScenario{FailureScenario::RightDisconnected, true}},
			Mcp));
		Tick(scheduler, 1, baseline);
		scheduler.Submit(Request(2, 2, {ReleaseControl{ControlChannel::Head}}));
		auto next = baseline;
		next.tracking.head = Pose(15);
		bool failed = false;
		try
		{
			scheduler.Apply({2}, {Duration{20}}, {Duration{120}}, next,
							[&](const auto&)
							{
								if (reject)
									throw std::bad_alloc{};
							});
		}
		catch (const std::bad_alloc&)
		{
			failed = true;
		}
		Require(failed == reject, "Release reservation result differs");
		if (!reject)
		{
			const auto explicitRelease = scheduler.Recorder().Snapshot().transactions.back();
			Require(explicitRelease.releaseState &&
						explicitRelease.releaseState->configuration.tracking.head.pose.position.x == 15 &&
						explicitRelease.releaseState->configuration.input.controllers[0].trigger == .75f,
					"Explicit release omitted its exact post-transaction state");
		}
		else
			Require(scheduler.Recorder().Snapshot().total == 2, "Failed boundary recorded speculative release state");
		scheduler.ReleaseSource(Manual);
		const auto released = scheduler.Recorder().Snapshot().transactions.back();
		Require(released.result == CommandResult::Released && released.afterFrame && released.releaseState &&
					released.releaseState->runtimeTime == released.appliedRuntimeTime &&
					released.releaseState->frame == FrameId{reject ? 1u : 2u},
				"Source release lost its boundary anchor");
		const auto& state = *released.releaseState;
		Require(state.configuration.tracking.head.pose.position.x == (reject ? 9 : 15) &&
					state.configuration.input.controllers[0].trigger == 0 &&
					state.scenarios[static_cast<std::size_t>(FailureScenario::RightDisconnected)] &&
					state.configuration.tracking.controllers[1].connected,
				"Release snapshot used an uncommitted baseline or baked in another owner's scenario");
		next.tracking.head = Pose(22);
		Tick(scheduler, 3, next);
		Require(state.configuration.tracking.head.pose.position.x == (reject ? 9 : 15),
				"Later boundary mutated a retained release snapshot");
	}
}
void SequenceReleaseRecording()
{
	for (bool stop : {false, true})
	{
		CommandScheduler scheduler;
		const auto baseline = SequenceBaseline();
		scheduler.StartRecording(baseline, {});
		scheduler.StartPoseSequence(Mcp, SequenceProgram(true));
		Tick(scheduler, 1, baseline);
		if (stop)
			scheduler.StopPoseSequence(Mcp);
		else
		{
			scheduler.Submit(Request(1, 2, {TakeControl{ControlChannel::Head, true}, SetHeadPose{Pose(7)}}));
			bool rejected = false;
			try
			{
				scheduler.Apply({2}, {Duration{20}}, {Duration{120}}, baseline,
								[](const auto&)
								{
									throw std::bad_alloc{};
								});
			}
			catch (const std::bad_alloc&)
			{
				rejected = true;
			}
			Require(rejected && scheduler.Recorder().Snapshot().total == 1,
					"Failed boundary published sequence release");
			Tick(scheduler, 2, baseline);
		}
		const auto recording = scheduler.Recorder().Snapshot();
		const auto& release = recording.transactions.back();
		Require(release.result == CommandResult::Released && release.afterFrame == stop &&
					release.appliedFrame == FrameId{stop ? 1u : 2u},
				"Sequence release lost boundary timing");
		Require(release.releaseState && release.releaseState->runtimeTime == release.appliedRuntimeTime,
				"Sequence cleanup omitted its fallback state or runtime anchor");
		Require(stop || std::none_of(release.transaction.commands.begin(), release.transaction.commands.end(),
									 [](const auto& command)
									 {
										 return std::get<ReleaseControl>(command).channel == ControlChannel::Head;
									 }),
				"Conflict cleanup released the new owner's channel");
	}
}
void RecordingOutcomeConsistency()
{
	CommandScheduler late;
	late.Start();
	Tick(late, 1);
	late.Recorder().Start({});
	const std::vector<CommandOutcome> full(1024);
	Require(late.Recorder().Observe(full), "Fill recorder before late admission");
	const auto outcome = late.Submit(Request(1, 1, {TakeControl{ControlChannel::Head}}));
	Require(outcome.result == CommandResult::Late && outcome.recordingOverrun &&
				late.Read(Manual, 1)->recordingOverrun && late.Recorder().Snapshot(0, 0).overflow,
			"Late reply hid committed recording overflow");
	for (auto capacity : {1u, 8u})
	{
		CommandScheduler scheduler(8, capacity);
		scheduler.Start();
		scheduler.Submit(Request(1, 1, {TakeControl{ControlChannel::Head}}));
		Require(scheduler.Cancel(Manual, 1) == CommandResult::Cancelled, "Prepare earlier cancellation");
		scheduler.Recorder().Start({});
		Require(scheduler.Recorder().Observe(full), "Fill recorder before source release");
		scheduler.Submit(Request(2, 1, {TakeControl{ControlChannel::Head}}));
		scheduler.Submit(Request(3, 1, {TakeControl{ControlChannel::Gaze}}));
		scheduler.ReleaseSource(Manual);
		Require(scheduler.Snapshot().pending == 0 && scheduler.Read(Manual, 3)->recordingOverrun &&
					scheduler.Recorder().Snapshot(0, 0).overflow,
				"Source-release cancellation hid recording overflow");
		if (capacity == 8)
			Require(!scheduler.Read(Manual, 1)->recordingOverrun && scheduler.Read(Manual, 2)->recordingOverrun,
					"Source release changed historical recording flags or missed a new cancellation");
	}
}

void ControllerProfilePathValidation()
{
	for (const std::string path : {"/interaction_profiles/vendor//controller", "/interaction_profiles/vendor/.",
								   "/interaction_profiles/vendor/..", "/interaction_profiles/.../controller"})
	{
		CommandState state;
		CommandConfiguration baseline;
		auto request = Request(1, 1,
							   {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(7)},
								TakeControl{ControlChannel::Profiles}, SetControllerProfile{0, path}});
		Require(state.Apply(request, baseline, {Duration{1}}) == CommandResult::Invalid &&
					state.Merge(baseline).input.preferredProfiles == baseline.input.preferredProfiles &&
					state.Merge(baseline).tracking.head.pose.position.x == baseline.tracking.head.pose.position.x &&
					!state.OwnedChannels(Manual)[static_cast<std::size_t>(ControlChannel::Profiles)] &&
					std::get<SetHeadPose>(request.commands[1]).value.pose.orientation.w == 1.005f,
				"Malformed controller profile committed command state or acknowledgements");
	}
}

void Transactions()
{
	CommandScheduler scheduler;
	scheduler.Start(173);
	auto request = Request(1, 2,
						   {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(3)}, TakeControl{ControlChannel::Gaze},
							SetGaze{{0.3f, 0.1f, true, true}}});
	Require(scheduler.Submit(request).result == CommandResult::Queued, "Queue exact transaction");
	Require(Tick(scheduler, 1)->configuration.tracking.head.pose.position.x == 0, "No early application");
	const auto applied = Tick(scheduler, 2);
	Require(applied->seed == 173 && applied->outcomes.size() == 1 &&
				applied->outcomes[0].result == CommandResult::Applied,
			"Acknowledged boundary");
	Require(applied->configuration.tracking.head.pose.position.x == 3 &&
				std::abs(applied->configuration.tracking.head.pose.orientation.w - 1) < 0.00001f,
			"Normalized pose applied");
	Require(scheduler.Submit(request).result == CommandResult::Applied, "Original retry after normalization");
	std::get<SetHeadPose>(request.commands[1]).value.pose.position.x = 7;
	Require(scheduler.Submit(request).result == CommandResult::Conflict, "Changed payload cannot retry ID");
	CommandConfiguration native;
	native.tracking.head.pose.position.x = -9;
	Require(scheduler.Merge(native).tracking.head.pose.position.x == 3, "Native baseline cannot overwrite control");
	auto invalid =
		Request(2, 3, {SetHeadPose{Pose(8)}, SetGaze{{std::numeric_limits<float>::quiet_NaN(), 0, true, true}}});
	scheduler.Submit(invalid);
	const auto failed = Tick(scheduler, 3);
	Require(failed->outcomes[0].result == CommandResult::Invalid &&
				failed->configuration.tracking.head.pose.position.x == 3,
			"Invalid transaction rolls back all fields");
	Require(applied->configuration.tracking.head.pose.position.x == 3 && applied->frame.value == 2,
			"Prior immutable reader survives");
	scheduler.Submit(Request(1, 4, {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(8)}}, Mcp));
	Require(Tick(scheduler, 4)->outcomes[0].result == CommandResult::Conflict, "Manual ownership blocks MCP");
	scheduler.Submit(Request(2, 5, {TakeControl{ControlChannel::Head, true}, SetHeadPose{Pose(8)}}, Mcp));
	Require(Tick(scheduler, 5)->configuration.tracking.head.pose.position.x == 8,
			"Explicit ownership replacement works");
	const auto last = scheduler.At({5});
	scheduler.ReleaseSource(Mcp);
	Require(scheduler.Merge(native).tracking.head.pose.position.x == -9 && last->owners[0] == Mcp,
			"Release source leaves historical ownership intact");
}
void TimeLateCancelBounds()
{
	CommandScheduler scheduler(2, 2);
	scheduler.Start();
	Tick(scheduler, 1);
	Require(scheduler.Submit(Request(1, 1, {TakeControl{ControlChannel::Gaze}})).result == CommandResult::Late,
			"Issued frame is late");
	auto late = Request(2, 1, {TakeControl{ControlChannel::Gaze}, SetGaze{{0.8f, 0, true, true}}});
	late.latePolicy = CommandLatePolicy::RescheduleNext;
	Require(scheduler.Submit(late).result == CommandResult::Rescheduled, "Explicit reschedule acknowledgement");
	Require(scheduler.Submit(late).result == CommandResult::Rescheduled, "Rescheduled original retry");
	auto timed = Request(3, 1, {SetGaze{{0.2f, 0, true, true}}});
	timed.target = {{}, SimulationTime{Duration{25}}};
	scheduler.Submit(timed);
	Require(scheduler.Submit(Request(4, 5, {SetGaze{{}}})).result == CommandResult::Capacity, "Bound pending queue");
	Require(Tick(scheduler, 2)->configuration.tracking.gaze.yaw == 0.8f, "Rescheduled applies at explicit next frame");
	Require(Tick(scheduler, 3)->configuration.tracking.gaze.yaw == 0.2f, "Time target first eligible boundary");
	Require(scheduler.Submit(Request(1, 1, {TakeControl{ControlChannel::Gaze}})).result == CommandResult::Conflict,
			"Evicted request cannot run again");
	Require(scheduler.Submit(Request(4, 8, {SetGaze{{}}})).result == CommandResult::Queued, "Capacity recovers");
	Require(scheduler.Cancel(Manual, 4) == CommandResult::Cancelled &&
				scheduler.Read(Manual, 4)->result == CommandResult::Cancelled,
			"Cancel future work");
	Require(scheduler.Submit(Request(5, 8, {SetGaze{{}}})).result == CommandResult::Queued, "Queue before close");
	scheduler.Close();
	Require(scheduler.Read(Manual, 5)->result == CommandResult::Cancelled && scheduler.Snapshot().pending == 0,
			"Close cancels unused work");
	Require(scheduler.Submit(Request(6, 9, {SetGaze{{}}})).result == CommandResult::Closed, "Closed rejects work");
}
void BodyViewsScenariosRecording()
{
	CommandConfiguration initial;
	initial.tracking.head.pose.position.x = 6;
	CommandScheduler scheduler;
	scheduler.Start(77, initial);
	scheduler.Recorder().Start({77, initial});
	bool rejected = false;
	try
	{
		scheduler.Recorder().Start({4});
	}
	catch (const std::logic_error&)
	{
		rejected = true;
	}
	Require(rejected, "Active recording cannot be overwritten");
	ViewSettings views;
	views.ipd = 0.08f;
	BodyControls body;
	body.preset = BodyPreset::Seated;
	TrackerDevice tracker{{2}, TrackerRole::Camera, true, false, {}, XrPosef{{0, 0, 0, 1}, {1, 2, 3}}};
	ControllerControls controls;
	controls.trigger = 0.9f;
	auto first =
		Request(1, 1,
				{TakeControl{ControlChannel::Body}, SetBodyControls{body}, TakeControl{ControlChannel::Views},
				 SetViewSettings{views}, TakeControl{ControlChannel::Proportions},
				 SetBodyProportions{{1.9f, 0.4f, 0.3f}}, TakeControl{ControlChannel::Trackers}, SetTracker{1, tracker},
				 TakeControl{ControlChannel::LeftControls}, SetControllerControls{0, controls},
				 TakeControl{ControlChannel::Head}, SetScenario{FailureScenario::HeadUntracked, true},
				 TakeControl{ControlChannel::LeftPose}, SetScenario{FailureScenario::LeftDisconnected, true}});
	scheduler.Submit(first);
	auto movedRoot = first;
	std::get<SetBodyControls>(movedRoot.commands[1]).value.rootPose = XrPosef{{0, 0, 0, 1}, {2, 0, 0}};
	Require(scheduler.Submit(movedRoot).result == CommandResult::Conflict,
			"Changed body root reused an existing command identity");
	const auto applied = Tick(scheduler, 1);
	Require(applied->outcomes[0].result == CommandResult::Applied &&
				applied->configuration.tracking.head.pose.position.x == 6,
			"Seeded initial state preserved by ownership claim");
	Require(applied->configuration.views.ipd == 0.08f && applied->configuration.proportions.height == 1.9f &&
				applied->configuration.tracking.body.preset == BodyPreset::Seated,
			"Body/view configuration transaction");
	Require(applied->configuration.tracking.trackers[1].role == TrackerRole::Camera &&
				!applied->configuration.tracking.trackers[1].tracked,
			"Stable tracker identity and untracked activity");
	Require(applied->configuration.tracking.head.flags == ValidPoseFlags &&
				!applied->configuration.input.controllers[0].connected &&
				!applied->configuration.tracking.controllers[0].connected,
			"Scenarios update sensor and device activity");
	scheduler.Submit(Request(
		2, 2,
		{SetScenario{FailureScenario::HeadUntracked, false}, SetScenario{FailureScenario::LeftDisconnected, false}}));
	const auto restored = Tick(scheduler, 2);
	Require(restored->configuration.tracking.head.flags == TrackedPoseFlags &&
				restored->configuration.input.controllers[0].connected,
			"Scenario recovery restores underlying state");
	scheduler.ReleaseSource(Manual);
	Require(scheduler.Merge({}).input.controllers[0].trigger == 0, "Stopping source releases held controls");
	scheduler.Recorder().Stop();
	const auto recording = scheduler.Recorder().Snapshot();
	Require(!recording.active && recording.origin.seed == 77 &&
				recording.origin.configuration.tracking.head.pose.position.x == 6 && recording.transactions.size() == 3,
			"Recording initial state and normalized acknowledged transactions");
	CommandRecorder tiny(1);
	tiny.Start({2});
	Require(tiny.Observe(recording.transactions[0]) && !tiny.Observe(recording.transactions[1]),
			"Recording overflow explicit");
	Require(!tiny.Snapshot().active && tiny.Snapshot().overflow && tiny.Snapshot().transactions.size() == 1,
			"Overflow never silently overwrites");
}
void DrainingRecording()
{
	CommandRecorder recorder(8);
	recorder.Start({}, true);
	std::size_t consumed = 0;
	for (std::size_t index = 0; index < 54000; ++index)
	{
		CommandOutcome outcome;
		outcome.transaction.id = index + 1;
		outcome.result = CommandResult::Applied;
		Require(recorder.Observe(outcome), "Promptly drained recording overflowed");
		if ((index + 1) % 8 != 0)
			continue;
		const auto page = recorder.Snapshot();
		Require(page.drain && page.active && !page.overflow && page.floor == consumed && page.total == index + 1 &&
					page.transactions.size() == 8,
				"Recording window counters changed across wraparound");
		for (const auto& value : page.transactions)
			Require(value.transaction.id == ++consumed, "Draining reordered or lost an outcome");
		recorder.Acknowledge(consumed);
		recorder.Acknowledge(consumed); // Retried acknowledgement is idempotent.
		Require(recorder.Snapshot().transactions.empty(), "Acknowledgement retained old outcomes");
	}
	bool stale = false, future = false;
	try
	{
		recorder.Snapshot(0);
	}
	catch (const std::out_of_range&)
	{
		stale = true;
	}
	try
	{
		recorder.Acknowledge(consumed + 1);
	}
	catch (const std::invalid_argument&)
	{
		future = true;
	}
	Require(stale && future, "Recording accepted an evicted read or future acknowledgement");
	CommandOutcome outcome;
	outcome.transaction.id = consumed + 1;
	bool rolledBack = false;
	try
	{
		recorder.ObservePrepared(std::span{&outcome, 1},
								 [&](bool complete)
								 {
									 Require(complete && recorder.Snapshot().total == consumed,
											 "Prepared recording leaked before publication");
									 throw std::runtime_error("reservation failure");
								 });
	}
	catch (const std::runtime_error&)
	{
		rolledBack = true;
	}
	Require(rolledBack && recorder.Snapshot().total == consumed && recorder.Observe(outcome),
			"Recording failed to roll back or retry a rejected publication");
	recorder.Stop();
	Require(!recorder.Snapshot().active && recorder.Snapshot().transactions.size() == 1,
			"Stopping discarded the unacknowledged tail");
	recorder.Acknowledge(++consumed);
	Require(recorder.Snapshot().transactions.empty(), "Stopped recording tail could not be acknowledged");
	recorder.Start({}, true);
	for (unsigned index = 0; index < 8; ++index)
		Require(recorder.Observe(outcome), "Bounded window filled too early");
	Require(!recorder.Observe(outcome) && recorder.Snapshot().overflow && !recorder.Snapshot().active &&
				recorder.Snapshot().total == 8,
			"Slow consumer silently overwrote its retained recording");
	recorder.Start({});
	bool legacy = false;
	try
	{
		recorder.Acknowledge(0);
	}
	catch (const std::invalid_argument&)
	{
		legacy = true;
	}
	Require(legacy && !recorder.Snapshot().drain && recorder.Snapshot().floor == 0 && recorder.Snapshot().total == 0,
			"Restart retained an old cursor or allowed legacy recording acknowledgement");
}
void AdmissionRollback()
{
	CommandScheduler commands;
	commands.Start();
	commands.Recorder().Start({});
	commands.Submit(Request(1, 1, {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(2)}}));
	FrameScheduler scheduler;
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	scheduler.Start(config);
	scheduler.Pause();
	Require(scheduler.GrantPermits() == XR_SUCCESS, "Reserve one command admission");
	FrameHistory history;
	history.StartGeneration();
	FramePrediction prediction;
	auto reserve = [&](const FramePrediction& candidate, bool fail)
	{
		commands.Apply(candidate.id, {Duration{1}}, candidate.displayTime, {},
					   [&](const auto&)
					   {
						   history.Issue(candidate,
										 [&](auto, auto)
										 {
											 if (fail)
												 throw std::bad_alloc{};
										 });
					   });
	};
	Require(scheduler.WaitFrame(prediction,
								[&](const auto& candidate)
								{
									reserve(candidate, true);
								}) == XR_ERROR_OUT_OF_MEMORY,
			"Reservation allocation failure is explicit");
	Require(!scheduler.Snapshot().waited && scheduler.Snapshot().permits == 1 && commands.Snapshot().pending == 1 &&
				!commands.Snapshot().applied && history.Snapshot().frames.empty() &&
				commands.Recorder().Snapshot().total == 0,
			"Failed command/history reservation preserves every gate and recorder");
	Require(scheduler.WaitFrame(prediction,
								[&](const auto& candidate)
								{
									reserve(candidate, false);
								}) == XR_SUCCESS &&
				prediction.id.value == 1,
			"Retry receives same reserved frame identity");
	Require(commands.At(prediction.id)->configuration.tracking.head.pose.position.x == 2 &&
				commands.Recorder().Snapshot().total == 1 && scheduler.Snapshot().permits == 0,
			"Successful retry commits command and recorder exactly once");
}

void RefinedBoundary()
{
	TrackingSystem tracking;
	TrackingState initial;
	const auto original = tracking.Publish({Duration{10}}, initial);
	initial.head.pose.position.x = 5;
	const auto refined = tracking.Publish({Duration{10}}, initial, true);
	Require(original.head.pose.position.x == 0 && refined.head.pose.position.x == 5 &&
				tracking.Sample({Duration{10}})->head.pose.position.x == 5,
			"Explicit same-boundary refinement immutable readers");
	bool duplicate = false;
	try
	{
		tracking.Publish({Duration{10}}, initial);
	}
	catch (const std::invalid_argument&)
	{
		duplicate = true;
	}
	Require(duplicate, "Ordinary duplicate timestamp remains invalid");
	bool previous = false;
	try
	{
		tracking.Publish({Duration{9}}, initial, true);
	}
	catch (const std::invalid_argument&)
	{
		previous = true;
	}
	Require(previous, "Refinement cannot rewrite earlier history");
}
void ScenarioRecoveryInSession()
{
	Instance instance(reinterpret_cast<XrInstance>(830), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(831), instance);
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	Require(session.Ready({Duration{1}}) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
			"Begin recovery session");
	session.Commands().Submit(
		Request(1, 1, {TakeControl{ControlChannel::LeftPose}, SetScenario{FailureScenario::LeftDisconnected, true}}));
	FramePrediction frame, ended;
	Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
				session.EndFrame(ended) == XR_SUCCESS,
			"Apply disconnection scenario");
	Require(!session.Input().Snapshot().controllers[0].connected &&
				!session.Tracking().Sample(frame.displayTime)->controllers[0].connected,
			"Disconnection affects input and tracking");
	session.Commands().Submit(Request(2, 2, {SetScenario{FailureScenario::LeftDisconnected, false}}));
	Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
				session.EndFrame(ended) == XR_SUCCESS,
			"Apply recovery scenario");
	Require(session.Input().Snapshot().controllers[0].connected &&
				session.Tracking().Sample(frame.displayTime)->controllers[0].connected,
			"Effective disconnection never contaminates the raw native baseline");
	session.Commands().Submit(
		Request(3, 3, {TakeControl{ControlChannel::Session}, SetScenario{FailureScenario::Unfocused, true}}));
	Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
				session.EndFrame(ended) == XR_SUCCESS,
			"Apply focus scenario");
	session.Scheduler().Pause();
	Require(session.Snapshot().state == XR_SESSION_STATE_VISIBLE, "Focus loss sends actual session transition");
	session.ReleaseCommandSource(Manual);
	Require(session.Snapshot().state == XR_SESSION_STATE_FOCUSED && session.Scheduler().Snapshot().paused,
			"Source release recovers focus while admission stays frozen");
	session.Close();
	CommandScheduler bounded(2, 1);
	bounded.Start();
	bounded.Submit(Request(1, 1, {TakeControl{ControlChannel::Gaze}}));
	Tick(bounded, 1);
	bounded.ReleaseSource(Manual);
	bounded.Submit(Request(2, 2, {TakeControl{ControlChannel::Gaze}}));
	Tick(bounded, 2);
	Require(bounded.Submit(Request(1, 1, {TakeControl{ControlChannel::Gaze}})).result == CommandResult::Conflict,
			"Release control preserves retired transaction floor beyond history eviction");
}

void ReferenceSpacePublicationAdmission()
{
	using namespace std::chrono_literals;
	Instance instance(reinterpret_cast<XrInstance>(815), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(816), instance);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(timing.epoch) == XR_SUCCESS, "Ready reference-space admission session");
	auto invalid = timing;
	invalid.epoch = {};
	Require(session.Begin(begin, invalid) == XR_ERROR_TIME_INVALID && !session.Snapshot().running &&
				!session.Snapshot().generation.value,
			"An invalid reference-space epoch admitted a partial session generation");
	Require(session.Begin(begin, timing) == XR_SUCCESS, "Begin reference-space admission session");
	session.Scheduler().Pause();
	auto wait = std::async(std::launch::async,
						   [&]
						   {
							   FramePrediction frame;
							   return session.WaitFrame(frame);
						   });
	Eventually(
		[&]
		{
			return session.Scheduler().Snapshot().waitPending;
		},
		"Frame did not wait for a permit");
	auto recording = std::async(std::launch::async,
								[&]
								{
									session.StartCommandRecording();
								});
	const bool capturedWhilePaused = recording.wait_for(1s) == std::future_status::ready;
	Require(session.Scheduler().GrantPermits() == XR_SUCCESS, "Grant reference-space admission permit");
	Require(wait.get() == XR_SUCCESS, "Admit paused reference-space frame");
	recording.get();
	Require(capturedWhilePaused && session.Commands().Recorder().Snapshot().origin.frame.value == 0,
			"Recording admission was blocked by frame pacing or captured an uncommitted origin");
	FramePrediction submitted;
	Require(session.BeginFrame() == XR_SUCCESS && session.EndFrame(submitted) == XR_SUCCESS,
			"Finish reference-space admission frame");

	ReferenceSpaceState future;
	future.local.position.x = 12;
	Require(session.Spaces().Restore({timing.epoch.value + timing.period * 4}, future) == XR_SUCCESS,
			"Install explicit future origin");
	Require(
		session.Commands().Submit(Request(1, 2, {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(3)}})).result ==
				CommandResult::Queued &&
			session.Scheduler().GrantPermits() == XR_SUCCESS,
		"Queue guarded origin boundary");
	FramePrediction rejected;
	Require(session.WaitFrame(rejected) == XR_ERROR_RUNTIME_FAILURE && session.Commands().Snapshot().pending == 1 &&
				!session.Commands().At({2}) && session.Commands().Recorder().Snapshot().total == 0,
			"An invalid origin boundary consumed commands or published a recording before failure");
}

void GenerationAndNativeBaseline()
{
	using namespace std::chrono_literals;
	Instance instance(reinterpret_cast<XrInstance>(820), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(821), instance);
	CommandConfiguration launch;
	launch.tracking.head.pose.position.x = 2;
	launch.views.ipd = 0.07f;
	launch.referenceSpaces.local.position.x = 6;
	launch.referenceSpaces.stageBounds.reset();
	Require(session.ConfigureCommands(44, launch, false) == XR_SUCCESS,
			"Configure non-overriding native launch baseline");
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	Require(session.Ready({Duration{1}}) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Begin baseline session");
	const auto generation = session.Snapshot().generation;
	Require(session.Commands().Snapshot().seed == 44 &&
				session.CommandConfigurationSnapshot().tracking.head.pose.position.x == 2,
			"Seed and launch settings observable before first wait");
	Require(session.Spaces().RecordingState(config.epoch).local.position.x == 6 &&
				!session.Spaces().RecordingState(config.epoch).stageBounds,
			"Launch configuration did not initialize reference-space origins and bounds");
	session.StartCommandRecording();
	FramePrediction prediction, ended;
	Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS, "Wait launch state");
	auto native = launch.tracking;
	native.head.pose.position.x = 5;
	auto optics = launch.views;
	optics.ipd = 0.08f;
	session.PublishTracking(prediction.displayTime, native, optics, prediction.id);
	Require(session.EndFrame(ended) == XR_SUCCESS, "Submit native refinement");
	const auto frame = session.History().Find({generation, prediction.id});
	Require(frame->submission->commands->configuration.tracking.head.pose.position.x == 5 &&
				frame->submission->commands->configuration.views.ipd == 0.08f,
			"Native baseline remains live without a command override");
	const auto recorded = session.Commands().Recorder().Snapshot();
	Require(recorded.origin.seed == 44 && recorded.origin.configuration.tracking.head.pose.position.x == 2 &&
				recorded.origin.configuration.views.ipd == 0.07f,
			"Recording retains actual pre-wait launch configuration");
	std::promise<void> entered, release;
	auto released = release.get_future();
	auto mutation =
		std::async(std::launch::async,
				   [&]
				   {
					   return session.WithCommandGeneration(generation,
															[&]
															{
																entered.set_value();
																released.wait();
																session.Commands().Submit(
																	Request(1, 2, {TakeControl{ControlChannel::Gaze}}));
															});
				   });
	entered.get_future().wait();
	Require(session.RequestExit(prediction.displayTime) == XR_SUCCESS, "Request exit during generation mutation");
	auto end = std::async(std::launch::async,
						  [&]
						  {
							  return session.End(prediction.displayTime);
						  });
	const auto waiting = end.wait_for(20ms);
	release.set_value();
	Require(waiting == std::future_status::timeout && mutation.get() && end.get() == XR_SUCCESS,
			"Lifecycle cannot restart across a bound command mutation");
	Require(session.Ready({Duration{1}}) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Restart generation");
	FramePrediction restarted;
	Require(session.WaitFrame(restarted) == XR_SUCCESS &&
				session.Spaces().RecordingState(restarted.displayTime).local.position.x == 6,
			"Restarted session could not publish against a reset reference-space generation");
	bool invoked = false;
	Require(!session.WithCommandGeneration(generation,
										   [&]
										   {
											   invoked = true;
										   }) &&
				!invoked,
			"Stale generation callback has no side effects");
	session.Close();
}

void PublicationBoundaryRollbackAndHistory()
{
	Instance instance(reinterpret_cast<XrInstance>(840), {}, XR_MAKE_VERSION(1, 0, 27));
	ActionSyncState actions(instance.Actions());
	SessionPublication publication(instance, actions);
	CommandConfiguration initial;
	initial.tracking.head.pose.position.x = 1;
	initial.views.ipd = 0.065f;
	publication.Configure(44, initial, false);
	SessionPublication::PreparedStart start(publication, {Duration{100}}, ViewLayout::StereoWithInset);
	start.Commit();
	publication.Commands().Recorder().Start({44, initial});
	Require(publication.Commands()
					.Submit(Request(1, 1,
									{TakeControl{ControlChannel::Head}, SetHeadPose{Pose(4)},
									 TakeControl{ControlChannel::LeftControls},
									 SetControllerControls{0, {.trigger = 0.8f}}}))
					.result == CommandResult::Queued,
			"Queue publication ownership changes");
	const FramePrediction first{{1}, {1}, {Duration{110}}, Duration{10}};
	SessionPublication::FrameBoundary boundary(publication, true, initial.referenceSpaces);
	bool rejected = false;
	try
	{
		boundary.Reserve(first,
						 [&](const AppliedCommandSnapshot&)
						 {
							 Require(publication.Commands().Recorder().Snapshot().total == 0,
									 "Reservation callback could not observe uncommitted recorder state");
							 throw std::bad_alloc{};
						 });
	}
	catch (const std::bad_alloc&)
	{
		rejected = true;
	}
	Require(rejected && publication.Commands().Snapshot().pending == 1 && !publication.Commands().At(first.id) &&
				!publication.Tracking().Sample(first.displayTime) &&
				publication.Input().Snapshot().controllers[0].trigger == 0 &&
				publication.Commands().Recorder().Snapshot().total == 0,
			"Rejected history reservation published commanded state or consumed the pending transaction");
	unsigned reservations = 0;
	boundary.Reserve(first,
					 [&](const AppliedCommandSnapshot&)
					 {
						 ++reservations;
					 });
	boundary.StateTransition(XR_SESSION_STATE_FOCUSED);
	boundary.Publish();
	const auto admitted = publication.Commands().At(first.id);
	Require(reservations == 1 && admitted && admitted->time.value == Duration{10} &&
				publication.Tracking().Sample(first.displayTime)->head.pose.position.x == 4 &&
				publication.Commands().Recorder().Snapshot().total == 1,
			"Retry did not publish exactly one matching command boundary");

	TrackingState native;
	native.head.pose.position.x = -20;
	native.controllers[1].pose.pose.position.x = 7;
	ViewSettings optics;
	optics.ipd = 0.071f;
	optics.outerWidth = 900;
	publication.PublishTracking(first.displayTime, native, optics, true);
	InputSnapshot input;
	input.time = first.displayTime;
	input.controllers[1].grip = 0.6f;
	Require(publication.PublishInput(input) == XR_SUCCESS &&
				publication.Input().Snapshot().controllers[0].trigger == 0.8f,
			"Native publication overrode a commanded channel");
	const auto originalViews = publication.ConfigurationAt(first.displayTime);
	Require(originalViews.Describe().size() == 4 && originalViews.Settings().outerWidth == 900,
			"Active inset optics were not published with native tracking");

	publication.Commands().Submit(Request(2, 2, {SetHeadPose{Pose(9)}}));
	const FramePrediction second{{2}, {2}, {Duration{120}}, Duration{10}};
	SessionPublication::FrameBoundary next(publication, true, initial.referenceSpaces);
	next.Reserve(second,
				 [](const AppliedCommandSnapshot&)
				 {
				 });
	next.Publish();
	optics.ipd = 0.079f;
	optics.outerWidth = 1200;
	publication.PublishTracking(second.displayTime, native, optics, true);
	FrameSubmissionState historical;
	historical.displayTime = first.displayTime;
	publication.CaptureFrame(first.id, historical);
	Require(historical.viewLayout == ViewLayout::StereoWithInset && historical.commands->frame == first.id &&
				historical.commands->configuration.tracking.head.pose.position.x == 4 &&
				historical.commands->configuration.tracking.controllers[1].pose.pose.position.x == 7 &&
				historical.commands->configuration.views.ipd == 0.071f && historical.viewSettings.outerWidth == 900 &&
				historical.commands->configuration.input.controllers[1].grip == 0.6f &&
				admitted->configuration.tracking.controllers[1].pose.pose.position.x != 7,
			"Older-frame metadata used newer optics/commands or mutated its earlier immutable reader");
	publication.ReleaseSource(Manual, true);
	Require(publication.Input().Snapshot().controllers[0].trigger == 0 &&
				publication.Input().Snapshot().controllers[0].connected &&
				publication.Input().Snapshot().controllers[1].grip == 0.6f,
			"Source retirement did not immediately neutralize only its owned controls");
	publication.Stop();
	Require(publication.Input().Snapshot().controllers[1].grip == 0 &&
				publication.Tracking().Sample(first.displayTime)->head.pose.position.x == 4,
			"Stop lost historical tracking or retained held controls");
	SessionPublication::PreparedStart restart(publication, {Duration{1000}}, ViewLayout::Stereo);
	restart.Commit();
	Require(!publication.Tracking().Sample(first.displayTime) &&
				publication.ConfigurationAt(first.displayTime).Describe().size() == 2 &&
				historical.commands->configuration.tracking.head.pose.position.x == 4 &&
				originalViews.Settings().outerWidth == 900,
			"Restart leaked prior publication history or changed retained readers");
}

void SessionBoundary()
{
	Instance instance(reinterpret_cast<XrInstance>(810), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(811), instance);
	Require(session.Ready({Duration{1}}) == XR_SUCCESS, "Ready commands session");
	Require(session.ConfigureCommands(999) == XR_SUCCESS, "Configure seed");
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
	begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	timing.epoch = {Duration{1000}};
	timing.period = Duration{10};
	Require(session.Begin(begin, timing) == XR_SUCCESS, "Begin commands session");
	session.Commands().Submit(
		Request(1, 1,
				{TakeControl{ControlChannel::Head}, SetHeadPose{Pose(4)}, TakeControl{ControlChannel::LeftControls},
				 SetControllerControls{0, {.trigger = 0.7f}}}));
	FramePrediction first, second, ended;
	Require(session.WaitFrame(first) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
			"Core-only wait applies commands");
	Require(session.Tracking().Sample(first.displayTime)->head.pose.position.x == 4, "Tracking without native publish");
	TrackingState native;
	native.head.pose.position.x = -20;
	native.controllers[1].pose.pose.position.x = 7;
	ViewSettings nativeViews;
	nativeViews.ipd = 0.071f;
	const auto admissionReader = session.Commands().At(first.id);
	session.PublishTracking(first.displayTime, native, nativeViews, first.id);
	InputSnapshot input;
	input.time = first.displayTime;
	input.controllers[1].grip = 0.6f;
	Require(session.PublishInput(input) == XR_SUCCESS && session.Input().Snapshot().controllers[0].trigger == 0.7f,
			"Native input cannot overwrite scheduled controls");
	Require(session.Tracking().Sample(first.displayTime)->head.pose.position.x == 4,
			"Native pose cannot overwrite scheduled head");
	session.Commands().Submit(Request(2, 2, {SetHeadPose{Pose(9)}}));
	Require(session.WaitFrame(second) == XR_SUCCESS, "Pipeline next issued frame");
	Require(session.EndFrame(ended) == XR_SUCCESS, "Submit earlier begun frame");
	const auto record = session.History().Find({session.Snapshot().generation, first.id});
	Require(record && record->submission && record->submission->commands &&
				record->submission->commands->frame == first.id &&
				record->submission->commands->configuration.tracking.head.pose.position.x == 4,
			"Submission retains matching earlier command snapshot");
	Require(record->submission->tracking->head.pose.position.x == 4, "Earlier historical tracking preserved");
	Require(record->submission->commands->configuration.tracking.controllers[1].pose.pose.position.x == 7 &&
				record->submission->commands->configuration.views.ipd == 0.071f &&
				record->submission->commands->configuration.input.controllers[1].grip == 0.6f &&
				admissionReader->configuration.tracking.controllers[1].pose.pose.position.x != 7,
			"Submission refines native-unowned values without mutating earlier readers");
	Require(record->submission->commands->time.value == first.displayTime.value - timing.epoch.value,
			"Commands record relative simulation time from the fixed epoch");
	session.Scheduler().Pause();
	session.Commands().Submit(Request(3, 3, {SetHeadPose{Pose(11)}}));
	session.StartCommandRecording();
	failCommandAllocation = true;
	session.ReleaseCommandSource(Manual);
	Require(!failCommandAllocation, "Injected cancellation history allocation was exercised");
	Require(session.Input().Snapshot().controllers[0].trigger == 0 &&
				session.Input().Snapshot().controllers[0].connected &&
				session.Input().Snapshot().controllers[1].grip == 0.6f,
			"Source disconnect releases held controls while frame admission stays paused");
	Require(session.Commands().Read(Manual, 3)->result == CommandResult::Cancelled &&
				session.Commands().Recorder().Snapshot().overflow && session.Commands().Snapshot().pending == 0,
			"Allocation failure still cancels abandoned work and reports incomplete recording");
	Require(session.BeginFrame() == XR_SUCCESS && session.EndFrame(ended) == XR_SUCCESS &&
				session.Scheduler().GrantPermits() == XR_SUCCESS,
			"Finish already admitted frame after cleanup");
	FramePrediction afterCleanup;
	Require(session.WaitFrame(afterCleanup) == XR_SUCCESS &&
				session.Tracking().Sample(afterCleanup.displayTime)->head.pose.position.x != 11,
			"Cancelled fallback command cannot execute on a later admitted frame");
	session.Close();
	Require(session.Commands().Snapshot().closed && session.Commands().Merge({}).input.controllers[0].trigger == 0,
			"Close releases command ownership");
	Require(record->submission->commands->seed == 999, "Retained applied state survives session close");
}
void ConditionalAppliedInputs()
{
	CommandScheduler scheduler;
	scheduler.Start();
	CommandConfiguration baseline;
	baseline.tracking.head = Pose(0);
	const auto observed = Tick(scheduler, 1, baseline);
	const CommandSource other{CommandSourceKind::Mcp, 303};
	auto head = Request(1, 2, {TakeControl{ControlChannel::Head}, SetHeadPose{Pose(7)}}, other);
	auto dependent = Request(1, 2, {TakeControl{ControlChannel::LeftControls}, SetControllerControls{0, {}}}, Mcp);
	dependent.expected.push_back(SetHeadPose{observed->configuration.tracking.head});
	Require(scheduler.Submit(head).result == CommandResult::Queued, "Queue concurrent head edit");
	Require(scheduler.Submit(dependent).result == CommandResult::Queued, "Queue dependent controller edit");
	Require(scheduler.Submit(dependent).result == CommandResult::Queued, "Identical conditional retry");
	auto changed = dependent;
	std::get<SetHeadPose>(changed.expected[0]).value.pose.position.x += 1;
	Require(scheduler.Submit(changed).result == CommandResult::Conflict, "Changed precondition must conflict on retry");
	const auto applied = Tick(scheduler, 2, baseline);
	Require(applied->outcomes.size() == 2 && applied->outcomes[0].result == CommandResult::Applied &&
				applied->outcomes[1].result == CommandResult::Conflict && !applied->owners[static_cast<std::size_t>(ControlChannel::LeftControls)],
			"Changed head must reject the complete dependent edit");
	auto control = Request(2, 3, {TakeControl{ControlChannel::LeftControls}, SetControllerControls{0, {}}}, Mcp);
	control.expected.push_back(SetControllerControls{0, applied->configuration.input.controllers[0]});
	Require(scheduler.Submit(control).result == CommandResult::Queued, "Queue edit against baseline controls");
	baseline.input.controllers[0].trigger = .5f;
	const auto updated = Tick(scheduler, 3, baseline);
	Require(updated->outcomes.size() == 1 && updated->outcomes[0].result == CommandResult::Conflict &&
				updated->configuration.input.controllers[0].trigger == .5f,
			"Changed UI baseline must reject the complete dependent edit");
}
} // namespace
int main()
{
	try
	{
		AllocationFreeSpaceCommit();
		AtomicCommandState();
		ConditionalAppliedInputs();
		ScheduledPoseSequence();
		SequenceLocalControllerFollowing();
		SequenceCaptureIssueRollback();
		SequenceTransfersCallerChannels();
		PoseSequenceReservationRollback();
		PoseSequenceFailureAndCaptureOnly();
		PreparedBoundaryAllocationRollback();
		ImmutableRecordingPublication();
		SourceReleaseRecording();
		SourceReleaseRecordingFailure();
		SequenceReleaseRecording();
		RecordedReleaseState();
		RecordingOutcomeConsistency();
		ControllerProfilePathValidation();
		LateAdmissionRollback();
		Transactions();
		TimeLateCancelBounds();
		BodyViewsScenariosRecording();
		DrainingRecording();
		AdmissionRollback();
		RefinedBoundary();
		ScenarioRecoveryInSession();
		GenerationAndNativeBaseline();
		ReferenceSpacePublicationAdmission();
		SessionBoundary();
		PublicationBoundaryRollbackAndHistory();
		std::cout << "Command scheduler tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
