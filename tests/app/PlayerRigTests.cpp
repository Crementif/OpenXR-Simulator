#include "../TestSupport.h"
#include "Math.h"
#include "app/PlayerRig.h"
#include "simulation/CommandScheduler.h"
#include "simulation/TrackingSystem.h"
#include <cmath>
#include <iostream>

namespace
{
using namespace simxr;
using namespace simxr::app;
void SamePose(XrPosef a, XrPosef b)
{
	const auto difference = RelativePose(a, b);
	Require(std::abs(difference.position.x) + std::abs(difference.position.y) + std::abs(difference.position.z) <
				.0001f,
			"Pose position changed during round trip");
	Require(std::abs(difference.orientation.w) > .99999f, "Pose orientation changed during round trip");
}
void RigComposition()
{
	ObservedSetup observed;
	observed.values.tracking.head.pose = {EulerAngles(.6f, .3f, -.2f), {3, 1.6f, -2}};
	observed.values.tracking.controllers[0].pose.pose = {EulerAngles(.1f, -.3f, .4f), {2.8f, 1.3f, -2.4f}};
	PlayerRig rig;
	rig.Reset(observed);
	SamePose(rig.Head(), observed.values.tracking.head.pose);
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		const auto initial = rig.Hand(hand);
		SamePose(initial, observed.values.tracking.controllers[hand].pose.pose);
		for (const auto anchor : {Anchor::Head, Anchor::World, Anchor::Body})
		{
			rig.SetHandAnchor(hand, anchor);
			SamePose(rig.Hand(hand), initial);
		}
	}
	const auto evaluated = TrackingSystem::Evaluate(RuntimeTime{Duration{1}}, observed.values.tracking,
													BodyModel{observed.values.proportions});
	for (std::size_t slot = 0; slot < 4; ++slot)
	{
		SamePose(rig.Tracker(slot), evaluated.trackers[slot].pose.pose);
		rig.AttachTracker(slot, BodyJoint::Head);
		SamePose(rig.Tracker(slot), evaluated.trackers[slot].pose.pose);
		rig.AttachTracker(slot, std::nullopt);
		SamePose(rig.Tracker(slot), evaluated.trackers[slot].pose.pose);
		rig.SnapTracker(slot);
		SamePose(rig.Tracker(slot), evaluated.trackers[slot].pose.pose);
	}
	const auto headOffset = RelativePose(rig.Head(), rig.Root());
	const auto handOffset = RelativePose(rig.Hand(0), rig.Root());
	rig.SetHandAnchor(1, Anchor::World);
	const auto stationary = rig.Hand(1);
	for (int frame = 0; frame < 90; ++frame)
		rig.Move({0, 0, -1}, .01f, 0, 1.0f / 90);
	SamePose(RelativePose(rig.Head(), rig.Root()), headOffset);
	SamePose(RelativePose(rig.Hand(0), rig.Root()), handOffset);
	SamePose(rig.Hand(1), stationary);
	const auto newWorld = XrPosef{EulerAngles(-.2f, .8f, .1f), {1, 2, 3}};
	rig.SetHandWorld(0, newWorld);
	SamePose(rig.Hand(0), newWorld);
}
void AtomicOwnership()
{
	ObservedSetup observed;
	PlayerRig rig;
	rig.Reset(observed);
	const CommandSource own{CommandSourceKind::Manual, 8};
	observed.owners[static_cast<std::size_t>(ControlChannel::LeftPose)] = CommandSource{CommandSourceKind::Manual, 9};
	observed.owners[static_cast<std::size_t>(ControlChannel::Head)] = own;
	rig.Move({1, 0, -1}, .2f, 0, .1f);
	const auto edit = rig.Transaction(observed, own);
	bool head = false, right = false;
	for (const auto& command : edit.commands)
	{
		if (std::holds_alternative<SetHeadPose>(command))
			head = true;
		if (const auto pose = std::get_if<SetControllerPose>(&command))
		{
			Require(pose->hand == 1, "Rig moved a device owned by another manual source");
			right = true;
		}
	}
	Require(head && right, "Owned head and available hand were not composed together");
	CommandScheduler scheduler;
	scheduler.Start();
	scheduler.Apply(FrameId{1}, SimulationTime{Duration{100}}, RuntimeTime{Duration{200}}, {});
	CommandTransaction transaction;
	transaction.id = 1;
	transaction.source = own;
	transaction.target.frame = FrameId{2};
	transaction.commands = edit.commands;
	Require(scheduler.Submit(transaction).result == CommandResult::Queued, "Rig transaction rejected");
	const auto applied = scheduler.Apply(FrameId{2}, SimulationTime{Duration{200}}, RuntimeTime{Duration{300}}, {});
	SamePose(applied->configuration.tracking.head.pose, rig.Head());
	SamePose(applied->configuration.tracking.controllers[1].pose.pose, rig.Hand(1));
	SamePose(applied->configuration.tracking.controllers[0].pose.pose,
			 observed.values.tracking.controllers[0].pose.pose);
	ObservedSetup moved = observed;
	moved.values.tracking.body.rootPose = XrPosef{EulerAngles(.8f, 0, 0), {5, 0, -4}};
	rig.Observe(moved);
	SamePose(rig.Root(), *moved.values.tracking.body.rootPose);
	SamePose(rig.Hand(1), moved.values.tracking.controllers[1].pose.pose);
	rig.Observe(observed);
}
// Applies an edit to the default configuration.
TrackingState Applied(const ManualTransaction& edit)
{
	CommandScheduler scheduler;
	scheduler.Start();
	scheduler.Apply(FrameId{1}, SimulationTime{Duration{100}}, RuntimeTime{Duration{200}}, {});
	CommandTransaction transaction;
	transaction.id = 1;
	transaction.source = CommandSource{CommandSourceKind::Manual, 3};
	transaction.target.frame = FrameId{2};
	transaction.commands = edit.commands;
	Require(scheduler.Submit(transaction).result == CommandResult::Queued, "Rig transaction rejected");
	const auto applied = scheduler.Apply(FrameId{2}, SimulationTime{Duration{200}}, RuntimeTime{Duration{300}}, {});
	Require(applied != nullptr, "Rig transaction not applied");
	return applied->configuration.tracking;
}
float Distance(XrVector3f a, XrVector3f b)
{
	return std::hypot(a.x - b.x, a.y - b.y, a.z - b.z);
}
void BodyPoses()
{
	ObservedSetup observed;
	PlayerRig rig;
	rig.Reset(observed);
	// Trackers start on their role joints and follow them natively.
	auto edit = rig.Transaction(observed, std::nullopt, true);
	for (const auto& command : edit.commands)
		if (const auto tracker = std::get_if<SetTracker>(&command))
			Require(tracker->value.anchor == BodyModel::TrackerJoint(tracker->value.role) &&
						!tracker->value.poseOverride,
					"Tracker did not start on its joint");
	for (const auto pose : {BodyPose::TPose, BodyPose::APose, BodyPose::Sit, BodyPose::Stand})
	{
		rig.ApplyBodyPose(pose);
		Require(rig.CurrentBodyPose() == pose, "Body pose not current after applying it");
		const auto tracking = Applied(rig.Transaction(observed, std::nullopt, true));
		const BodyModel model{observed.values.proportions};
		const auto evaluated = TrackingSystem::Evaluate(RuntimeTime{Duration{1}}, tracking, model);
		for (std::size_t hand = 0; hand < 2; ++hand)
		{
			const auto palm = evaluated.body.joints[static_cast<std::size_t>(hand ? BodyJoint::RightHandPalm
																				  : BodyJoint::LeftHandPalm)]
								  .pose.position;
			Require(Distance(evaluated.controllers[hand].pose.pose.position, palm) < .03f,
					"Posed hand is not on the posed palm");
			if (pose == BodyPose::TPose)
			{
				const auto upper = hand ? BodyJoint::RightArmUpper : BodyJoint::LeftArmUpper;
				const auto shoulder = evaluated.body.joints[static_cast<std::size_t>(upper)].pose.position;
				Require(std::abs(palm.y - shoulder.y) < .05f && std::abs(palm.x - shoulder.x) > .4f,
						"T-pose arms are not straight out");
			}
		}
		for (std::size_t slot = 0; slot < 4; ++slot)
		{
			const auto joint = BodyModel::TrackerJoint(tracking.trackers[slot].role);
			Require(Distance(evaluated.trackers[slot].pose.pose.position,
							 evaluated.body.joints[static_cast<std::size_t>(joint)].pose.position) < .001f,
					"Tracker left its joint after a body pose");
			SamePose(rig.Tracker(slot), evaluated.trackers[slot].pose.pose);
		}
	}
}
void JointOffsetsFollowBody()
{
	ObservedSetup observed;
	PlayerRig rig;
	rig.Reset(observed);
	const XrPosef offset{EulerAngles(.2f, -.3f, .1f), {.12f, .08f, -.04f}};
	rig.SetTrackerOffset(0, offset);
	auto tracking = Applied(rig.Transaction(observed, std::nullopt, true));
	Require(tracking.trackers[0].jointOffset.has_value() && !tracking.trackers[0].poseOverride,
			"Attached offset was frozen in world space");
	// No further tracker command: application body animation must move the tracker.
	for (const auto preset : {BodyPreset::Standing, BodyPreset::Seated, BodyPreset::TPose})
	{
		tracking.body.preset = preset;
		tracking.body.rootPose = XrPosef{EulerAngles(.7f, 0, 0), {2, 0, -3}};
		const auto frame = TrackingSystem::Evaluate(RuntimeTime{Duration{1}}, tracking, BodyModel{});
		const auto joint = *tracking.trackers[0].anchor;
		SamePose(frame.trackers[0].pose.pose,
				 Compose(frame.body.joints[static_cast<std::size_t>(joint)].pose, offset));
		observed.values.tracking = frame.input;
		rig.Reset(observed);
		SamePose(rig.Tracker(0), frame.trackers[0].pose.pose);
	}
}
void RapidUndo()
{
	ObservedSetup original;
	UndoStack undo;
	const auto edit = [](float x)
	{
		ManualTransaction result;
		PoseSample pose;
		pose.pose.position.x = x;
		result.commands.emplace_back(SetHeadPose{pose});
		return result;
	};
	undo.Record(original, edit(1));
	undo.Record(original, edit(2));
	const auto first = undo.Undo(original, std::nullopt);
	Require(first.has_value(), "First rapid edit undo missing");
	Require(Applied(*first).head.pose.position.x == 1, "Rapid edits lost their authored intermediate state");
	const auto second = undo.Undo(original, std::nullopt);
	Require(second && Applied(*second).head.pose.position.x == 0, "Second undo did not restore origin");
	undo.Record(original, edit(3));
	Require(Applied(*undo.Undo(original, std::nullopt)).head.pose.position.x == 0,
			"Edit after undo reused a stale authored state");
}
void Undo()
{
	ObservedSetup before;
	PlayerRig rig;
	rig.Reset(before);
	UndoStack undo;
	rig.Preset(0, HandPreset::Up);
	auto drag = rig.Transaction(before, std::nullopt);
	undo.Record(before, drag);
	ObservedSetup during = before;
	during.values.tracking = Applied(drag);
	undo.Record(during, drag);
	drag.continuous = false;
	undo.Record(during, drag);
	ManualTransaction input;
	input.commands.emplace_back(SetControllerControls{0, ControllerControls{}});
	undo.Record(during, input);
	const auto restore = undo.Undo(during, std::nullopt);
	Require(restore.has_value() && undo.Empty(), "A drag is not one undo step");
	Require(Distance(during.values.tracking.controllers[0].pose.pose.position,
					 before.values.tracking.controllers[0].pose.pose.position) > .1f,
			"The preset did not move the hand");
	bool hand = false;
	for (const auto& command : restore->commands)
		if (const auto pose = std::get_if<SetControllerPose>(&command); pose && pose->hand == 0)
		{
			SamePose(pose->value.pose.pose, before.values.tracking.controllers[0].pose.pose);
			hand = true;
		}
	Require(hand, "Undo did not restore the hand");
	Require(!undo.Undo(during, std::nullopt), "Undo past the first edit");
}
} // namespace
int main()
{
	try
	{
		RigComposition();
		AtomicOwnership();
		BodyPoses();
		JointOffsetsFollowBody();
		Undo();
		RapidUndo();
		std::cout << "PASS Player rig composition, inverse, anchors, body poses, undo and atomic ownership\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
