#include "../TestSupport.h"
#include "runtime/Session.h"

#include <iostream>

namespace simxr::tests
{
void TestAutomationLifetime()
{
	Instance instance(reinterpret_cast<XrInstance>(1), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(2), instance);
	FramePool pool(4);
	Require(session.ConfigureCapturePool(pool) == XR_SUCCESS, "Configure native automation capture pool");
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	const CommandSource owner{CommandSourceKind::Rig, 17}, other{CommandSourceKind::Mcp, 18};
	const auto program = std::make_shared<const PoseSequence>(PoseSequence::Parse(
		R"({"frame":0,"head":{"x":2},"capture":true}
{"frame":1,"head":{"x":4},"capture":true})",
		true));
	for (unsigned run = 0; run < 2; ++run)
	{
		Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
				"Begin native automation run");
		auto& commands = session.Commands();
		Require(session.StartCommandRecording(true, owner) == RecordingResult::Ok &&
					session.StartCommandRecording(true, owner) == RecordingResult::Conflict &&
					session.StartCommandRecording(true, other) == RecordingResult::Busy,
				"Recording ownership did not share the native command source");
		const auto recording = commands.Recorder().Snapshot({}, 0).identity;
		const auto sequence = commands.StartPoseSequence(owner, program);
		Require(sequence.has_value(), "Native sequence could not share its owner's recording");
		Require(commands.Submit({1,
								 owner,
								 {FrameId{4}, {}},
								 CommandLatePolicy::Reject,
								 {TakeControl{ControlChannel::Gaze}, SetGaze{{}}}})
						.result == CommandResult::Queued,
				"Queue a rig transaction alongside native playback");
		for (unsigned index = 1; index <= 3; ++index)
		{
			FramePrediction frame;
			Require(session.WaitFrame(frame) == XR_SUCCESS && frame.id.value == index,
					"Automation frame identity changed");
			const auto metadata = session.History().Find({session.Snapshot().generation, frame.id});
			Require(metadata->sequenceCapture && metadata->sequenceCapture->request.owner == owner &&
						metadata->sequenceCapture->request.sequence == sequence->id &&
						metadata->sequenceCapture->reservation.result == SnapshotAdmission::Accepted &&
						commands.At(frame.id)->configuration.tracking.head.pose.position.x == (index == 2 ? 4 : 2),
					"Shared ownership changed loop timing or exact capture receipts");
			Require(session.BeginFrame() == XR_SUCCESS && session.EndFrame(frame) == XR_SUCCESS,
					"Submit native automation frame");
		}
		Require(commands.StopRecording(other, recording) == RecordingResult::Busy &&
					commands.AcknowledgeRecording(other, recording, 1) == RecordingResult::Busy,
				"Another source could stop or acknowledge the recording");
		session.ReleaseCommandSource(other, true);
		Require(commands.Recorder().Snapshot({}, 0).active && commands.PoseSequenceStatusFor(owner)->active &&
					session.Captures().Snapshot().captures.size() == 3,
				"Foreign cleanup touched owned automation");
		if (!run)
		{
			// Source release is the shared completion/cancellation/disconnect path.
			session.ReleaseCommandSource(owner, true);
			Require(!commands.Recorder().Snapshot({}, 0).active && !commands.PoseSequenceStatusFor(owner)->active &&
						commands.Read(owner, 1)->result == CommandResult::Cancelled &&
						session.Captures().Snapshot().captures.empty(),
					"Source cleanup split automation lifetimes");
			const auto recorded = commands.Recorder().Snapshot();
			Require(!recorded.transactions.empty() && recorded.identity == recording &&
						recorded.transactions.back().result == CommandResult::Released &&
						recorded.transactions.back().releaseState,
					"Source cleanup discarded recording evidence");
			Require(session.StartCommandRecording(true, other) == RecordingResult::Ok,
					"Released recording owner blocked a successor");
			const auto successor = commands.Recorder().Snapshot({}, 0).identity;
			session.ReleaseCommandSource(owner, true);
			Require(commands.Recorder().Snapshot({}, 0).active && successor != recording,
					"Repeated old cleanup stopped the successor");
			Require(commands.StopRecording(other, successor) == RecordingResult::Ok &&
						commands.AcknowledgeRecording(other, successor, 0) == RecordingResult::Ok,
					"Stopped draining recording lost its owner's acknowledgement rights");
		}
		Require(session.RequestExit(timing.epoch) == XR_SUCCESS && session.End(timing.epoch) == XR_SUCCESS &&
					!commands.Recorder().Snapshot({}, 0).active && session.Captures().Snapshot().captures.empty(),
				"Session end retained automation or owned capture tickets");
	}
	session.Close();
}
}

int main()
{
	try
	{
		simxr::tests::TestAutomationLifetime();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
