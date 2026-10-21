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
		rig.SetTrackerAnchor(slot, Anchor::Head);
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
} // namespace
int main()
{
	try
	{
		RigComposition();
		AtomicOwnership();
		std::cout << "PASS Player rig composition, inverse, anchors and atomic ownership\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
