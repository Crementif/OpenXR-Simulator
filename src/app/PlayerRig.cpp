#include "app/PlayerRig.h"
#include "Math.h"
#include "simulation/TrackingSystem.h"
#include <algorithm>
#include <cmath>
#include <numbers>

namespace simxr::app
{
namespace
{
float Yaw(XrQuaternionf q)
{
	const auto forward = Rotate(q, {0, 0, -1});
	return std::atan2(-forward.x, -forward.z);
}
} // namespace
XrPosef PlayerRig::Root() const
{
	return {EulerAngles(facing, 0, 0), position};
}
XrPosef PlayerRig::Head() const
{
	return Compose(Root(),
				   {Multiply(EulerAngles(m_headYaw, m_headPitch, 0), m_headRoll), {m_lean.x, height, m_lean.z}});
}
XrPosef PlayerRig::AnchorPose(Anchor anchor) const
{
	switch (anchor)
	{
	case Anchor::Body:
		return Root();
	case Anchor::Head:
		return Head();
	case Anchor::World:
		return IdentityPose();
	}
	return IdentityPose();
}
XrPosef PlayerRig::Hand(std::size_t hand) const
{
	const auto& device = m_hands.at(hand);
	return Compose(AnchorPose(device.anchor), device.offset);
}
XrPosef PlayerRig::Tracker(std::size_t slot) const
{
	const auto& device = m_trackers.at(slot);
	return Compose(AnchorPose(device.anchor), device.offset);
}
void PlayerRig::SetHeadWorld(XrPosef pose)
{
	const auto localHead = RelativePose(pose, Root());
	height = localHead.position.y;
	m_lean = localHead.position;
	m_headYaw = Yaw(localHead.orientation);
	const auto forward = Rotate(localHead.orientation, {0, 0, -1});
	m_headPitch = std::asin(std::clamp(forward.y, -1.0f, 1.0f));
	m_headRoll = RelativePose({localHead.orientation, {}}, {EulerAngles(m_headYaw, m_headPitch, 0), {}}).orientation;
}
void PlayerRig::SetHandWorld(std::size_t hand, XrPosef pose)
{
	auto& device = m_hands.at(hand);
	device.offset = RelativePose(pose, AnchorPose(device.anchor));
}
void PlayerRig::SetTrackerWorld(std::size_t slot, XrPosef pose)
{
	auto& device = m_trackers.at(slot);
	device.offset = RelativePose(pose, AnchorPose(device.anchor));
}
void PlayerRig::SetHandAnchor(std::size_t hand, Anchor anchor)
{
	const auto pose = Hand(hand);
	m_hands.at(hand).anchor = anchor;
	SetHandWorld(hand, pose);
}
void PlayerRig::SetTrackerAnchor(std::size_t slot, Anchor anchor)
{
	const auto pose = Tracker(slot);
	m_trackers.at(slot).anchor = anchor;
	SetTrackerWorld(slot, pose);
}
void PlayerRig::Reset(const ObservedSetup& observed)
{
	const auto& head = observed.values.tracking.head.pose;
	position = {head.position.x, 0, head.position.z};
	facing = Yaw(head.orientation);
	if (const auto& root = observed.values.tracking.body.rootPose)
	{
		position = root->position;
		facing = Yaw(root->orientation);
	}
	m_hands = {};
	m_trackers = {};
	m_ready = true;
	Observe(observed);
}
void PlayerRig::Observe(const ObservedSetup& observed)
{
	if (!m_ready)
	{
		Reset(observed);
		return;
	}
	if (const auto& root = observed.values.tracking.body.rootPose)
	{
		position = root->position;
		facing = Yaw(root->orientation);
	}
	else
	{
		const auto head = observed.values.tracking.head.pose;
		position = {head.position.x, 0, head.position.z};
		facing = Yaw(head.orientation);
	}
	SetHeadWorld(observed.values.tracking.head.pose);
	for (std::size_t hand = 0; hand < 2; ++hand)
		SetHandWorld(hand, observed.values.tracking.controllers[hand].pose.pose);
	// Evaluate the same deterministic model as the runtime, including joint overrides and proportions.
	const auto evaluated = TrackingSystem::Evaluate(RuntimeTime{Duration{1}}, observed.values.tracking,
													BodyModel{observed.values.proportions});
	for (std::size_t slot = 0; slot < m_trackers.size(); ++slot)
		SetTrackerWorld(slot, evaluated.trackers[slot].pose.pose);
}
void PlayerRig::Preset(std::size_t hand, HandPreset preset)
{
	const float side = hand ? 1.0f : -1.0f;
	XrPosef offset = IdentityPose();
	offset.position = {side * .24f, height - .55f, -.30f};
	if (preset == HandPreset::Forward)
		offset.position = {side * .20f, height - .15f, -.55f};
	if (preset == HandPreset::Up)
		offset.orientation = EulerAngles(0, std::numbers::pi_v<float> / 2, 0);
	if (preset == HandPreset::TPose)
		offset.position = {side * .65f, height - .20f, 0};
	SetHandWorld(hand, Compose(Root(), offset));
}
void PlayerRig::Move(XrVector3f velocity, float yaw, float pitch, float seconds, bool freeLook)
{
	if (freeLook)
		m_headYaw = std::clamp(m_headYaw + yaw, -2.0943951f, 2.0943951f);
	else
		facing += yaw;
	m_headPitch = std::clamp(m_headPitch + pitch, -1.553343f, 1.553343f);
	const float delta = std::clamp(seconds, 0.0f, .1f) * walkSpeed;
	const auto movement = Rotate(Root().orientation, {velocity.x, 0, velocity.z});
	position.x += movement.x * delta;
	position.z += movement.z * delta;
	height = std::clamp(height + velocity.y * delta, .4f, 2.2f);
}
ManualTransaction PlayerRig::Transaction(const ObservedSetup& observed, std::optional<CommandSource> ownSource,
										 bool includeWorld) const
{
	ManualTransaction result;
	result.continuous = true;
	const auto allowed = [&](ControlChannel channel)
	{
		const auto& owner = observed.owners[static_cast<std::size_t>(channel)];
		return !owner || (ownSource && *owner == *ownSource);
	};
	if (allowed(ControlChannel::Head))
	{
		auto head = observed.values.tracking.head;
		head.pose = Head();
		result.commands.emplace_back(TakeControl{ControlChannel::Head, false});
		result.commands.emplace_back(SetHeadPose{head});
	}
	for (std::uint32_t hand = 0; hand < 2; ++hand)
	{
		const auto channel = hand ? ControlChannel::RightPose : ControlChannel::LeftPose;
		if (!allowed(channel) || (!includeWorld && m_hands[hand].anchor == Anchor::World))
			continue;
		auto controller = observed.values.tracking.controllers[hand];
		controller.pose.pose = Hand(hand);
		result.commands.emplace_back(TakeControl{channel, false});
		result.commands.emplace_back(SetControllerPose{hand, controller});
	}
	if (allowed(ControlChannel::Body) && observed.bodyRootAvailable)
	{
		auto body = observed.values.tracking.body;
		body.rootPose = Root();
		result.commands.emplace_back(TakeControl{ControlChannel::Body, false});
		result.commands.emplace_back(SetBodyControls{body});
	}
	if (allowed(ControlChannel::Trackers) && observed.trackerAttachmentsAvailable)
	{
		bool claimed = false;
		for (std::uint32_t slot = 0; slot < m_trackers.size(); ++slot)
		{
			if (!includeWorld && m_trackers[slot].anchor == Anchor::World)
				continue;
			if (!claimed)
			{
				result.commands.emplace_back(TakeControl{ControlChannel::Trackers, false});
				claimed = true;
			}
			auto tracker = observed.values.tracking.trackers[slot];
			tracker.poseOverride = Tracker(slot);
			result.commands.emplace_back(SetTracker{slot, tracker});
		}
	}
	return result;
}
} // namespace simxr::app
