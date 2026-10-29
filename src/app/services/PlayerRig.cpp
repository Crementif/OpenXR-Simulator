#include "app/services/PlayerRig.h"
#include "Pch.h"
#include "app/services/SpatialMath.h"
#include "math/Math.h"
#include "simulation/BodyModel.h"

namespace simxr::app
{
static float Yaw(XrQuaternionf q)
{
	const auto forward = Rotate(q, {0, 0, -1});
	return std::atan2(-forward.x, -forward.z);
}

static XrVector3f Add(XrVector3f a, XrVector3f b)
{
	return {a.x + b.x, a.y + b.y, a.z + b.z};
}

static XrVector3f Subtract(XrVector3f a, XrVector3f b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

static XrVector3f Scale(XrVector3f a, float factor)
{
	return {a.x * factor, a.y * factor, a.z * factor};
}

static float Dot(XrVector3f a, XrVector3f b)
{
	return a.x * b.x + a.y * b.y + a.z * b.z;
}

static float Length(XrVector3f a)
{
	return std::sqrt(Dot(a, a));
}

static XrVector3f Normalize(XrVector3f a)
{
	const float length = Length(a);
	return length > 1e-6f ? Scale(a, 1 / length) : XrVector3f{0, 0, -1};
}

static std::size_t Index(BodyJoint joint)
{
	return static_cast<std::size_t>(joint);
}

struct ArmJoints
{
	BodyJoint upper, lower, twist, wrist, palm, firstFinger, lastFinger, upperLeg, lowerLeg;
};

static constexpr std::array<ArmJoints, 2> s_arms{{
	{BodyJoint::LeftArmUpper, BodyJoint::LeftArmLower, BodyJoint::LeftHandWristTwist, BodyJoint::LeftHandWrist,
	 BodyJoint::LeftHandPalm, BodyJoint::LeftHandIndexDistal, BodyJoint::LeftHandThumbTip, BodyJoint::LeftUpperLeg,
	 BodyJoint::LeftLowerLeg},
	{BodyJoint::RightArmUpper, BodyJoint::RightArmLower, BodyJoint::RightHandWristTwist, BodyJoint::RightHandWrist,
	 BodyJoint::RightHandPalm, BodyJoint::RightHandIndexDistal, BodyJoint::RightHandThumbTip, BodyJoint::RightUpperLeg,
	 BodyJoint::RightLowerLeg},
}};
static constexpr std::array s_attachable{
	BodyJoint::Hips,		   BodyJoint::Chest,		BodyJoint::SpineMiddle,	  BodyJoint::Neck,
	BodyJoint::Head,		   BodyJoint::LeftShoulder, BodyJoint::RightShoulder, BodyJoint::LeftArmUpper,
	BodyJoint::RightArmUpper,  BodyJoint::LeftArmLower, BodyJoint::RightArmLower, BodyJoint::LeftHandWrist,
	BodyJoint::RightHandWrist, BodyJoint::LeftHandPalm, BodyJoint::RightHandPalm, BodyJoint::LeftUpperLeg,
	BodyJoint::RightUpperLeg,  BodyJoint::LeftLowerLeg, BodyJoint::RightLowerLeg, BodyJoint::LeftFootAnkle,
	BodyJoint::RightFootAnkle, BodyJoint::LeftFootBall, BodyJoint::RightFootBall};

// Arm pose from the posed skeleton: the elbow bends back and down when the hand is closer than full reach.
static void PoseArm(const BodySample& base, const ArmJoints& arm, XrVector3f palm, BodyControls& body)
{
	const auto at = [&](BodyJoint joint) {
		return base.joints[Index(joint)].pose.position;
	};
	const auto upper = at(arm.upper), lower = at(arm.lower), twist = at(arm.twist), wrist = at(arm.wrist),
			   bindPalm = at(arm.palm);
	const float upperLength = Length(Subtract(lower, upper)), lowerLength = Length(Subtract(wrist, lower)),
				handLength = Length(Subtract(bindPalm, wrist));
	const auto reachDirection = Normalize(Subtract(palm, upper));
	auto target = Subtract(palm, Scale(reachDirection, handLength));
	const float distance = std::min(Length(Subtract(target, upper)), (upperLength + lowerLength) * .999f);
	target = Add(upper, Scale(reachDirection, distance));
	const float along = (upperLength * upperLength - lowerLength * lowerLength + distance * distance) / (2 * distance);
	const float bend = std::sqrt(std::max(0.0f, upperLength * upperLength - along * along));
	XrVector3f preferred{0, -.3f, 1};
	preferred = Normalize(Subtract(preferred, Scale(reachDirection, Dot(preferred, reachDirection))));
	const auto elbow = Add(Add(upper, Scale(reachDirection, along)), Scale(preferred, bend));
	const auto upperRotation = RotationBetween(Normalize(Subtract(lower, upper)), Normalize(Subtract(elbow, upper)));
	const auto lowerRotation = RotationBetween(Normalize(Subtract(wrist, lower)), Normalize(Subtract(target, elbow)));
	const auto handRotation = RotationBetween(Normalize(Subtract(bindPalm, wrist)), Normalize(Subtract(palm, target)));
	const auto set = [&](BodyJoint joint, XrVector3f position, XrQuaternionf orientation) {
		body.joints[Index(joint)] = BodyJointOverride{XrPosef{orientation, position}, true};
	};
	const float twistWeight = Length(Subtract(twist, lower)) / std::max(lowerLength, 1e-4f);
	set(arm.upper, upper, upperRotation);
	set(arm.lower, elbow, lowerRotation);
	set(arm.twist, Add(elbow, Scale(Subtract(target, elbow), twistWeight)), lowerRotation);
	set(arm.wrist, target, handRotation);
	set(arm.palm, palm, handRotation);
	for (auto index = Index(arm.firstFinger); index <= Index(arm.lastFinger); ++index)
		if (index != Index(arm.palm))
			set(static_cast<BodyJoint>(index),
				Add(palm, Rotate(handRotation, Subtract(base.joints[index].pose.position, bindPalm))), handRotation);
}

const char* TrackerRoleName(TrackerRole role)
{
	switch (role)
	{
	case TrackerRole::Waist:
		return "Waist";
	case TrackerRole::Chest:
		return "Chest";
	case TrackerRole::LeftFoot:
		return "Left foot";
	case TrackerRole::RightFoot:
		return "Right foot";
	case TrackerRole::Unassigned:
		return "Unassigned";
	case TrackerRole::HandheldObject:
		return "Handheld object";
	case TrackerRole::LeftShoulder:
		return "Left shoulder";
	case TrackerRole::RightShoulder:
		return "Right shoulder";
	case TrackerRole::LeftElbow:
		return "Left elbow";
	case TrackerRole::RightElbow:
		return "Right elbow";
	case TrackerRole::LeftKnee:
		return "Left knee";
	case TrackerRole::RightKnee:
		return "Right knee";
	case TrackerRole::LeftWrist:
		return "Left wrist";
	case TrackerRole::RightWrist:
		return "Right wrist";
	case TrackerRole::LeftAnkle:
		return "Left ankle";
	case TrackerRole::RightAnkle:
		return "Right ankle";
	case TrackerRole::Camera:
		return "Camera";
	case TrackerRole::Keyboard:
		return "Keyboard";
	}
	return "Tracker";
}

char TrackerRoleLetter(TrackerRole role)
{
	return TrackerRoleName(role)[0];
}

const char* BodyJointName(BodyJoint joint)
{
	switch (joint)
	{
	case BodyJoint::Hips:
		return "Hips";
	case BodyJoint::Chest:
		return "Chest";
	case BodyJoint::SpineMiddle:
		return "Spine";
	case BodyJoint::Neck:
		return "Neck";
	case BodyJoint::Head:
		return "Head";
	case BodyJoint::LeftShoulder:
		return "Left shoulder";
	case BodyJoint::RightShoulder:
		return "Right shoulder";
	case BodyJoint::LeftArmUpper:
		return "Left upper arm";
	case BodyJoint::RightArmUpper:
		return "Right upper arm";
	case BodyJoint::LeftArmLower:
		return "Left forearm";
	case BodyJoint::RightArmLower:
		return "Right forearm";
	case BodyJoint::LeftHandWrist:
		return "Left wrist";
	case BodyJoint::RightHandWrist:
		return "Right wrist";
	case BodyJoint::LeftHandPalm:
		return "Left palm";
	case BodyJoint::RightHandPalm:
		return "Right palm";
	case BodyJoint::LeftUpperLeg:
		return "Left thigh";
	case BodyJoint::RightUpperLeg:
		return "Right thigh";
	case BodyJoint::LeftLowerLeg:
		return "Left knee";
	case BodyJoint::RightLowerLeg:
		return "Right knee";
	case BodyJoint::LeftFootAnkle:
		return "Left ankle";
	case BodyJoint::RightFootAnkle:
		return "Right ankle";
	case BodyJoint::LeftFootBall:
		return "Left toes";
	case BodyJoint::RightFootBall:
		return "Right toes";
	default:
		return "Joint";
	}
}

std::span<const BodyJoint> AttachableJoints()
{
	return s_attachable;
}

PosedBody PoseBody(BodyPose pose, const BodyControls& current, const BodyProportions& proportions, float headHeight)
{
	PosedBody result;
	result.body = current;
	result.body.rootPose.reset();
	result.body.joints = {};
	result.body.preset = pose == BodyPose::Sit ? BodyPreset::Seated : BodyPreset::Standing;
	auto controls = result.body;
	controls.rootPose = IdentityPose();
	const PoseSample head{{{0, 0, 0, 1}, {0, headHeight, 0}}, TrackedPoseFlags};
	const auto base = BodyModel{proportions}.Evaluate(head, controls, RuntimeTime{Duration{1}});
	const auto at = [&](BodyJoint joint) {
		return base.joints[Index(joint)].pose.position;
	};
	for (std::size_t side = 0; side < 2; ++side)
	{
		const auto& arm = s_arms[side];
		const float sign = side ? 1.0f : -1.0f;
		auto palm = at(arm.palm);
		const float reach = Length(Subtract(at(arm.lower), at(arm.upper))) +
							Length(Subtract(at(arm.wrist), at(arm.lower))) +
							Length(Subtract(at(arm.palm), at(arm.wrist)));
		const auto upper = at(arm.upper);
		if (pose == BodyPose::TPose)
			palm = Add(upper, {sign * reach * .98f, 0, 0});
		else if (pose == BodyPose::APose)
			palm = Add(upper, Scale(XrVector3f{sign * .7071f, -.7071f, 0}, reach * .98f));
		else if (pose == BodyPose::Sit)
		{
			const auto thigh = Add(at(arm.upperLeg), Scale(Subtract(at(arm.lowerLeg), at(arm.upperLeg)), .6f));
			palm = Add(thigh, {sign * .05f, .1f, 0});
		}
		if (pose != BodyPose::Stand)
			PoseArm(base, arm, palm, result.body);
		result.hands[side] = {{0, 0, 0, 1}, palm};
	}
	return result;
}

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

XrPosef PlayerRig::JointPose(BodyJoint joint) const
{
	auto controls = m_body;
	controls.active = true;
	controls.rootPose = m_bodyRoot ? std::optional{Root()} : std::nullopt;
	const PoseSample head{Head(), TrackedPoseFlags};
	return BodyModel{m_proportions}.Evaluate(head, controls, RuntimeTime{Duration{1}}).joints[Index(joint)].pose;
}

XrPosef PlayerRig::Tracker(std::size_t slot) const
{
	const auto& tracker = m_trackers.at(slot);
	return tracker.joint ? Compose(JointPose(*tracker.joint), tracker.offset) : tracker.offset;
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
	auto& tracker = m_trackers.at(slot);
	tracker.offset = tracker.joint ? RelativePose(pose, JointPose(*tracker.joint)) : pose;
}

void PlayerRig::SetHandAnchor(std::size_t hand, Anchor anchor)
{
	const auto pose = Hand(hand);
	m_hands.at(hand).anchor = anchor;
	SetHandWorld(hand, pose);
}

void PlayerRig::AttachTracker(std::size_t slot, std::optional<BodyJoint> joint)
{
	const auto pose = Tracker(slot);
	m_trackers.at(slot).joint = joint;
	SetTrackerWorld(slot, pose);
}

void PlayerRig::SnapTracker(std::size_t slot)
{
	m_trackers.at(slot) = {BodyModel::TrackerJoint(m_trackerDevices.at(slot).role), IdentityPose()};
}

void PlayerRig::SetTrackerOffset(std::size_t slot, XrPosef offset)
{
	m_trackers.at(slot).offset = offset;
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
	if (observed.values.tracking.trackers.size() > MaxTrackerDevices)
		throw std::invalid_argument("Tracker inventory exceeds capacity");
	m_trackerDevices = observed.values.tracking.trackers;
	m_hands = {};
	m_trackers.resize(observed.values.tracking.trackers.size());
	for (std::size_t slot = 0; slot < m_trackers.size(); ++slot)
		m_trackers[slot] = {BodyModel::TrackerJoint(observed.values.tracking.trackers[slot].role), IdentityPose()};
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
	m_body = observed.values.tracking.body;
	m_body.rootPose.reset();
	m_proportions = observed.values.proportions;
	m_bodyRoot = observed.bodyRootAvailable;
	const auto& left = m_body.joints[Index(BodyJoint::LeftHandPalm)];
	const auto& upper = m_body.joints[Index(BodyJoint::LeftArmUpper)];
	if (m_body.preset == BodyPreset::Seated)
		m_bodyPose = BodyPose::Sit;
	else if (!left || !left->pose || !upper || !upper->pose)
		m_bodyPose = m_body.preset == BodyPreset::TPose ? BodyPose::TPose : BodyPose::Stand;
	else
	{
		const auto arm = Subtract(left->pose->position, upper->pose->position);
		m_bodyPose = -arm.y < std::abs(arm.x) * .35f ? BodyPose::TPose : BodyPose::APose;
	}
	ObserveTrackers(observed);
}

void PlayerRig::ObserveTrackers(const ObservedSetup& observed)
{
	if (observed.values.tracking.trackers.size() > MaxTrackerDevices)
		throw std::invalid_argument("Tracker inventory exceeds capacity");
	std::vector<TrackerAttachment> attachments;
	attachments.reserve(observed.values.tracking.trackers.size());
	for (const auto& device : observed.values.tracking.trackers)
	{
		const auto previous = std::find_if(m_trackerDevices.begin(), m_trackerDevices.end(),
										   [&](const auto& value) { return value.identity == device.identity; });
		const auto joint = device.anchor.value_or(BodyModel::TrackerJoint(device.role));
		TrackerAttachment tracker{joint, IdentityPose()};
		if (previous != m_trackerDevices.end())
		{
			const auto index = static_cast<std::size_t>(previous - m_trackerDevices.begin());
			if (index < m_trackers.size())
				tracker = m_trackers[index];
		}
		if (device.jointOffset)
			tracker = {joint, *device.jointOffset};
		else if (!device.poseOverride)
			tracker = {joint, IdentityPose()};
		else if (!tracker.joint)
			tracker.offset = *device.poseOverride;
		else
			tracker = {joint, RelativePose(*device.poseOverride, JointPose(joint))};
		attachments.push_back(tracker);
	}
	m_trackerDevices = observed.values.tracking.trackers;
	m_trackers = std::move(attachments);
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
	SetHandWorld(hand, Compose(Root(), offset));
}

void PlayerRig::ApplyBodyPose(BodyPose pose)
{
	const auto posed = PoseBody(pose, m_body, m_proportions, height);
	m_body = posed.body;
	for (std::size_t hand = 0; hand < 2; ++hand)
		m_hands[hand] = {Anchor::Body, posed.hands[hand]};
	m_bodyPose = pose;
}

void PlayerRig::ResetHead()
{
	m_headYaw = m_headPitch = 0;
	m_headRoll = {0, 0, 0, 1};
	m_lean = {};
	height = m_proportions.height;
}

void PlayerRig::ResetHand(std::size_t hand)
{
	m_hands.at(hand) = {Anchor::Body, TrackingState{}.controllers.at(hand).pose.pose};
}

void PlayerRig::ResetBody()
{
	m_body = {};
	m_bodyPose = BodyPose::Stand;
}

void PlayerRig::ResetTracker(std::size_t slot)
{
	const auto identity = m_trackerDevices.at(slot).identity;
	const auto role = m_trackerDevices.at(slot).role;
	const auto sourcePath = m_trackerDevices.at(slot).sourcePath;
	m_trackerDevices.at(slot) = TrackerDevice{identity, role};
	m_trackerDevices.at(slot).sourcePath = sourcePath;
	SnapTracker(slot);
}

void PlayerRig::ResetAll()
{
	position = {};
	facing = 0;
	ResetHead();
	ResetBody();
	for (std::size_t hand = 0; hand < m_hands.size(); ++hand)
		ResetHand(hand);
	for (std::size_t slot = 0; slot < m_trackers.size(); ++slot)
		ResetTracker(slot);
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
	const auto allowed = [&](ControlChannel channel) {
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
	if (allowed(ControlChannel::Body) && (observed.bodyRootAvailable || observed.bodyOverridesAvailable))
	{
		auto body = m_body;
		body.rootPose = observed.bodyRootAvailable ? std::optional{Root()} : std::nullopt;
		if (!observed.bodyOverridesAvailable)
			body.joints = observed.values.tracking.body.joints;
		result.commands.emplace_back(TakeControl{ControlChannel::Body, false});
		result.commands.emplace_back(SetBodyControls{body});
	}
	if (allowed(ControlChannel::Trackers) && observed.trackerAttachmentsAvailable)
	{
		bool claimed = false;
		auto devices = m_trackerDevices;
		for (std::uint32_t slot = 0; slot < m_trackers.size(); ++slot)
		{
			const auto& attachment = m_trackers[slot];
			if (!includeWorld && !attachment.joint)
				continue;
			if (!claimed)
			{
				result.commands.emplace_back(TakeControl{ControlChannel::Trackers, false});
				claimed = true;
			}
			auto tracker = m_trackerDevices[slot];
			if (attachment.joint)
				tracker.anchor = attachment.joint;
			tracker.jointOffset = attachment.joint ? std::optional{attachment.offset} : std::nullopt;
			tracker.poseOverride = attachment.joint ? std::nullopt : std::optional{Tracker(slot)};
			devices[slot] = tracker;
			if (devices.size() <= 4)
				result.commands.emplace_back(SetTracker{slot, tracker});
		}
		if (claimed && devices.size() > 4)
			result.commands.emplace_back(SetTrackers{std::move(devices)});
	}
	return result;
}
} // namespace simxr::app
