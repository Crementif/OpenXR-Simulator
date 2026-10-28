#include "../TestSupport.h"
#include "app/services/PlayerRig.h"
#include "app/services/UndoStack.h"
#include "math/Math.h"
#include "simulation/CommandScheduler.h"
#include "simulation/TrackingSystem.h"
#include <algorithm>
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
			const auto palm =
				evaluated.body
					.joints[static_cast<std::size_t>(hand ? BodyJoint::RightHandPalm : BodyJoint::LeftHandPalm)]
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
	for (const auto preset : {BodyPreset::Standing, BodyPreset::Seated, BodyPreset::TPose})
	{
		tracking.body.preset = preset;
		tracking.body.rootPose = XrPosef{EulerAngles(.7f, 0, 0), {2, 0, -3}};
		const auto frame = TrackingSystem::Evaluate(RuntimeTime{Duration{1}}, tracking, BodyModel{});
		const auto joint = *tracking.trackers[0].anchor;
		SamePose(frame.trackers[0].pose.pose, Compose(frame.body.joints[static_cast<std::size_t>(joint)].pose, offset));
		observed.values.tracking = frame.input;
		rig.Reset(observed);
		SamePose(rig.Tracker(0), frame.trackers[0].pose.pose);
	}
}
void ExpandedRig()
{
	ObservedSetup observed;
	observed.values.tracking.trackers.clear();
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
	{
		TrackerDevice device{{100 + index}, TrackerRole::Unassigned};
		device.sourcePath = TrackerSourcePath{"htcx:/device/" + std::to_string(index)};
		device.poseOverride = XrPosef{{0, 0, 0, 1}, {static_cast<float>(index), 1, 0}};
		observed.values.tracking.trackers.push_back(device);
	}
	PlayerRig rig;
	rig.Reset(observed);
	rig.AttachTracker(63, std::nullopt);
	std::reverse(observed.values.tracking.trackers.begin(), observed.values.tracking.trackers.end());
	rig.Observe(observed);
	Require(rig.TrackerDeviceAt(0).identity.value == 163 && !rig.TrackerAt(0).joint,
			"Tracker attachment followed a slot instead of its identity");
	SamePose(rig.Tracker(0), *observed.values.tracking.trackers[0].poseOverride);
	rig.ResetTracker(63);
	Require(rig.TrackerDeviceAt(63).sourcePath.View() == "htcx:/device/0",
			"Reset erased physical device provenance");
	const auto edit = rig.Transaction(observed, std::nullopt, true);
	Require(edit.commands.size() < 64 && std::holds_alternative<SetTrackers>(edit.commands.back()) &&
				std::get<SetTrackers>(edit.commands.back()).value.size() == 64,
			"Whole rig update exceeded the command bound or truncated trackers");
	observed.values.tracking.trackers.clear();
	rig.Observe(observed);
	rig.ResetAll();
	const auto empty = rig.Transaction(observed, std::nullopt, true);
	Require(std::none_of(empty.commands.begin(), empty.commands.end(),
						 [](const auto& command)
						 {
							 return std::holds_alternative<SetTracker>(command);
						 }),
			"Empty rig retained tracker slots");
}
void TrackerCollectionUndo()
{
	ObservedSetup original;
	original.values.tracking.trackers.clear();
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
		original.values.tracking.trackers.push_back({{100 + index}, TrackerRole::Unassigned});
	UndoStack undo;
	undo.Observe(original);
	ManualTransaction edit;
	auto moved = original.values.tracking.trackers.back();
	moved.poseOverride = XrPosef{{0, 0, 0, 1}, {3, 2, 1}};
	edit.commands = {SetTracker{63, moved}};
	undo.Record(original, edit);
	auto current = original;
	current.values.tracking.trackers[63] = moved;
	undo.Observe(current);
	const auto restored = undo.Undo(current, std::nullopt);
	Require(restored && std::get<SetTracker>(restored->commands.back()).index == 63 &&
				!std::get<SetTracker>(restored->commands.back()).value.poseOverride,
			"Expanded tracker slot could not be undone");
	undo.Observe(original);
	edit.commands = {SetTrackers{}};
	undo.Record(original, edit);
	current.values.tracking.trackers.clear();
	undo.Observe(current);
	Require(undo.Changes().back().label != "No change", "Inventory removal had no change description");
	const auto collection = undo.Undo(current, std::nullopt);
	Require(collection && std::get<SetTrackers>(collection->commands.back()).value.size() == MaxTrackerDevices,
			"Collection undo did not restore removed devices");
	undo.Observe(original);
	auto reversed = original.values.tracking.trackers;
	std::reverse(reversed.begin(), reversed.end());
	edit.commands = {SetTrackers{reversed}};
	undo.Record(original, edit);
	edit.commands = {SetTrackers{}};
	undo.Record(original, edit);
	const auto grouped = undo.Undo(current, std::nullopt, 2);
	Require(grouped && std::get<SetTrackers>(grouped->commands.back()).value.front().identity.value == 100 &&
				std::get<SetTrackers>(grouped->commands.back()).value.back().identity.value == 163,
			"Grouped topology undo did not restore the oldest ordered inventory");
	undo.Observe(original);
	edit.commands = {SetTracker{63, moved}};
	undo.Record(original, edit);
	current = original;
	current.values.tracking.trackers.back().identity.value = 900;
	undo.Observe(current);
	Require(!undo.Undo(current, std::nullopt) && !undo.Empty(),
			"Slot undo overwrote another device after topology changed");
	current.values.tracking.trackers.clear();
	undo.Observe(current);
	Require(!undo.Undo(current, std::nullopt) && !undo.Empty(),
			"Slot undo consumed history after the device was removed");
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
void HistoryUndo()
{
	ObservedSetup original;
	UndoStack undo;
	ManualTransaction head;
	PoseSample moved;
	moved.pose.position.x = 1;
	head.commands.emplace_back(SetHeadPose{moved});
	undo.Record(original, head);
	ManualTransaction hand;
	auto controller = original.values.tracking.controllers[1];
	controller.pose.pose.position.z -= .3f;
	hand.commands.emplace_back(SetControllerPose{1, controller});
	undo.Record(original, hand);
	ManualTransaction height;
	BodyProportions taller;
	taller.height = 1.8f;
	height.commands.emplace_back(SetBodyProportions{taller});
	undo.Record(original, height);
	const auto changes = undo.Changes();
	Require(changes.size() == 3 && changes[0].label == "Head moved" && changes[1].label == "Right hand moved" &&
				changes[2].label == "Height 1.70 \u2192 1.80 m",
			"History did not name each change");
	const auto restore = undo.Undo(original, std::nullopt, 2);
	Require(restore.has_value(), "Undo to a history entry produced nothing");
	const auto state = Applied(*restore);
	Require(std::none_of(restore->commands.begin(), restore->commands.end(),
						 [](const auto& command)
						 {
							 return std::holds_alternative<SetHeadPose>(command);
						 }),
			"Undoing two steps also rewrote the step before them");
	SamePose(state.controllers[1].pose.pose, original.values.tracking.controllers[1].pose.pose);
	Require(undo.Changes().size() == 1 && undo.Changes()[0].label == "Head moved",
			"Undo to an entry kept the undone changes");
}
void UndoOwnershipAndScope()
{
	ObservedSetup before;
	UndoStack undo;
	ManualTransaction gaze;
	gaze.commands.emplace_back(SetGaze{GazeInput{.4f, .2f, true, true}});
	undo.Record(before, gaze);
	ManualTransaction tracker;
	auto moved = before.values.tracking.trackers[1];
	moved.anchor.reset();
	moved.poseOverride = XrPosef{EulerAngles(.2f, 0, 0), {3, 2, 1}};
	tracker.commands.emplace_back(SetTracker{1, moved});
	undo.Record(before, tracker);
	const auto changes = undo.Changes();
	ObservedSetup current = before;
	current.values.tracking.gaze = {.4f, .2f, true, true};
	current.values.tracking.trackers[1] = moved;
	current.values.tracking.head.pose.position.x = 7;
	current.values.tracking.trackers[3].connected = false;
	undo.Observe(current);
	Require(undo.Changes()[1].label == changes[1].label,
			"A later runtime observation changed an earlier edit's description");
	const CommandSource other{CommandSourceKind::Rig, 2};
	current.owners[static_cast<std::size_t>(ControlChannel::Trackers)] = other;
	Require(!undo.Undo(current, std::nullopt, 2) && undo.Changes().size() == 2,
			"Blocked undo discarded history or partially restored a grouped edit");
	current.owners[static_cast<std::size_t>(ControlChannel::Trackers)].reset();
	current.scenarios[static_cast<std::size_t>(FailureScenario::GazeUnavailable)] = true;
	Require(!undo.Undo(current, std::nullopt, 2) && undo.Changes().size() == 2,
			"Undo overwrote an active failure scenario or discarded blocked history");
	current.scenarios = {};
	current.owners[static_cast<std::size_t>(ControlChannel::Head)] = other;
	const auto restore = undo.Undo(current, std::nullopt, 2);
	Require(restore && undo.Empty(), "An unrelated owner blocked undo");
	Require(restore->commands.size() == 4, "Undo restored channels outside its edit groups");
	for (const auto& command : restore->commands)
	{
		if (const auto value = std::get_if<SetTracker>(&command))
			Require(value->index == 1 && value->value.anchor == before.values.tracking.trackers[1].anchor &&
						!value->value.poseOverride,
					"Undo restored the wrong tracker slot");
		if (const auto value = std::get_if<SetGaze>(&command))
			Require(value->value.yaw == 0 && value->value.pitch == 0, "Undo lost the oldest selected gaze state");
	}
	ManualTransaction head;
	auto pose = current.values.tracking.head;
	pose.pose.position.x = 8;
	head.commands.emplace_back(SetHeadPose{pose});
	undo.Record(before, head);
	current.owners[static_cast<std::size_t>(ControlChannel::Head)].reset();
	const auto next = undo.Undo(current, std::nullopt);
	Require(next && Applied(*next).head.pose.position.x == 7,
			"Selective undo replaced unrelated authored state with an older snapshot");
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
		ExpandedRig();
		TrackerCollectionUndo();
		RapidUndo();
		HistoryUndo();
		UndoOwnershipAndScope();
		std::cout << "PASS Player rig composition, inverse, anchors, body poses, undo and atomic ownership\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
