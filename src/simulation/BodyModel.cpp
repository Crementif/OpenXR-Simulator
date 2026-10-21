#include "simulation/BodyModel.h"
#include "Math.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <limits>
#include <stdexcept>

namespace simxr
{
namespace
{
constexpr std::size_t Index(BodyJoint joint)
{
	return static_cast<std::size_t>(joint);
}

const std::array<BodyJoint, XR_BODY_JOINT_COUNT_FB> s_jointsFB = []
{
	std::array<BodyJoint, XR_BODY_JOINT_COUNT_FB> result{};
	result[XR_BODY_JOINT_ROOT_FB] = BodyJoint::Root;
	result[XR_BODY_JOINT_HIPS_FB] = BodyJoint::Hips;
	result[XR_BODY_JOINT_SPINE_LOWER_FB] = BodyJoint::SpineLower;
	result[XR_BODY_JOINT_SPINE_MIDDLE_FB] = BodyJoint::SpineMiddle;
	result[XR_BODY_JOINT_SPINE_UPPER_FB] = BodyJoint::SpineUpper;
	result[XR_BODY_JOINT_CHEST_FB] = BodyJoint::Chest;
	result[XR_BODY_JOINT_NECK_FB] = BodyJoint::Neck;
	result[XR_BODY_JOINT_HEAD_FB] = BodyJoint::Head;
	result[XR_BODY_JOINT_LEFT_SHOULDER_FB] = BodyJoint::LeftShoulder;
	result[XR_BODY_JOINT_LEFT_SCAPULA_FB] = BodyJoint::LeftScapula;
	result[XR_BODY_JOINT_LEFT_ARM_UPPER_FB] = BodyJoint::LeftArmUpper;
	result[XR_BODY_JOINT_LEFT_ARM_LOWER_FB] = BodyJoint::LeftArmLower;
	result[XR_BODY_JOINT_LEFT_HAND_WRIST_TWIST_FB] = BodyJoint::LeftHandWristTwist;
	result[XR_BODY_JOINT_RIGHT_SHOULDER_FB] = BodyJoint::RightShoulder;
	result[XR_BODY_JOINT_RIGHT_SCAPULA_FB] = BodyJoint::RightScapula;
	result[XR_BODY_JOINT_RIGHT_ARM_UPPER_FB] = BodyJoint::RightArmUpper;
	result[XR_BODY_JOINT_RIGHT_ARM_LOWER_FB] = BodyJoint::RightArmLower;
	result[XR_BODY_JOINT_RIGHT_HAND_WRIST_TWIST_FB] = BodyJoint::RightHandWristTwist;
	result[XR_BODY_JOINT_LEFT_HAND_PALM_FB] = BodyJoint::LeftHandPalm;
	result[XR_BODY_JOINT_LEFT_HAND_WRIST_FB] = BodyJoint::LeftHandWrist;
	result[XR_BODY_JOINT_LEFT_HAND_THUMB_METACARPAL_FB] = BodyJoint::LeftHandThumbMetacarpal;
	result[XR_BODY_JOINT_LEFT_HAND_THUMB_PROXIMAL_FB] = BodyJoint::LeftHandThumbProximal;
	result[XR_BODY_JOINT_LEFT_HAND_THUMB_DISTAL_FB] = BodyJoint::LeftHandThumbDistal;
	result[XR_BODY_JOINT_LEFT_HAND_THUMB_TIP_FB] = BodyJoint::LeftHandThumbTip;
	result[XR_BODY_JOINT_LEFT_HAND_INDEX_METACARPAL_FB] = BodyJoint::LeftHandIndexMetacarpal;
	result[XR_BODY_JOINT_LEFT_HAND_INDEX_PROXIMAL_FB] = BodyJoint::LeftHandIndexProximal;
	result[XR_BODY_JOINT_LEFT_HAND_INDEX_INTERMEDIATE_FB] = BodyJoint::LeftHandIndexIntermediate;
	result[XR_BODY_JOINT_LEFT_HAND_INDEX_DISTAL_FB] = BodyJoint::LeftHandIndexDistal;
	result[XR_BODY_JOINT_LEFT_HAND_INDEX_TIP_FB] = BodyJoint::LeftHandIndexTip;
	result[XR_BODY_JOINT_LEFT_HAND_MIDDLE_METACARPAL_FB] = BodyJoint::LeftHandMiddleMetacarpal;
	result[XR_BODY_JOINT_LEFT_HAND_MIDDLE_PROXIMAL_FB] = BodyJoint::LeftHandMiddleProximal;
	result[XR_BODY_JOINT_LEFT_HAND_MIDDLE_INTERMEDIATE_FB] = BodyJoint::LeftHandMiddleIntermediate;
	result[XR_BODY_JOINT_LEFT_HAND_MIDDLE_DISTAL_FB] = BodyJoint::LeftHandMiddleDistal;
	result[XR_BODY_JOINT_LEFT_HAND_MIDDLE_TIP_FB] = BodyJoint::LeftHandMiddleTip;
	result[XR_BODY_JOINT_LEFT_HAND_RING_METACARPAL_FB] = BodyJoint::LeftHandRingMetacarpal;
	result[XR_BODY_JOINT_LEFT_HAND_RING_PROXIMAL_FB] = BodyJoint::LeftHandRingProximal;
	result[XR_BODY_JOINT_LEFT_HAND_RING_INTERMEDIATE_FB] = BodyJoint::LeftHandRingIntermediate;
	result[XR_BODY_JOINT_LEFT_HAND_RING_DISTAL_FB] = BodyJoint::LeftHandRingDistal;
	result[XR_BODY_JOINT_LEFT_HAND_RING_TIP_FB] = BodyJoint::LeftHandRingTip;
	result[XR_BODY_JOINT_LEFT_HAND_LITTLE_METACARPAL_FB] = BodyJoint::LeftHandLittleMetacarpal;
	result[XR_BODY_JOINT_LEFT_HAND_LITTLE_PROXIMAL_FB] = BodyJoint::LeftHandLittleProximal;
	result[XR_BODY_JOINT_LEFT_HAND_LITTLE_INTERMEDIATE_FB] = BodyJoint::LeftHandLittleIntermediate;
	result[XR_BODY_JOINT_LEFT_HAND_LITTLE_DISTAL_FB] = BodyJoint::LeftHandLittleDistal;
	result[XR_BODY_JOINT_LEFT_HAND_LITTLE_TIP_FB] = BodyJoint::LeftHandLittleTip;
	result[XR_BODY_JOINT_RIGHT_HAND_PALM_FB] = BodyJoint::RightHandPalm;
	result[XR_BODY_JOINT_RIGHT_HAND_WRIST_FB] = BodyJoint::RightHandWrist;
	result[XR_BODY_JOINT_RIGHT_HAND_THUMB_METACARPAL_FB] = BodyJoint::RightHandThumbMetacarpal;
	result[XR_BODY_JOINT_RIGHT_HAND_THUMB_PROXIMAL_FB] = BodyJoint::RightHandThumbProximal;
	result[XR_BODY_JOINT_RIGHT_HAND_THUMB_DISTAL_FB] = BodyJoint::RightHandThumbDistal;
	result[XR_BODY_JOINT_RIGHT_HAND_THUMB_TIP_FB] = BodyJoint::RightHandThumbTip;
	result[XR_BODY_JOINT_RIGHT_HAND_INDEX_METACARPAL_FB] = BodyJoint::RightHandIndexMetacarpal;
	result[XR_BODY_JOINT_RIGHT_HAND_INDEX_PROXIMAL_FB] = BodyJoint::RightHandIndexProximal;
	result[XR_BODY_JOINT_RIGHT_HAND_INDEX_INTERMEDIATE_FB] = BodyJoint::RightHandIndexIntermediate;
	result[XR_BODY_JOINT_RIGHT_HAND_INDEX_DISTAL_FB] = BodyJoint::RightHandIndexDistal;
	result[XR_BODY_JOINT_RIGHT_HAND_INDEX_TIP_FB] = BodyJoint::RightHandIndexTip;
	result[XR_BODY_JOINT_RIGHT_HAND_MIDDLE_METACARPAL_FB] = BodyJoint::RightHandMiddleMetacarpal;
	result[XR_BODY_JOINT_RIGHT_HAND_MIDDLE_PROXIMAL_FB] = BodyJoint::RightHandMiddleProximal;
	result[XR_BODY_JOINT_RIGHT_HAND_MIDDLE_INTERMEDIATE_FB] = BodyJoint::RightHandMiddleIntermediate;
	result[XR_BODY_JOINT_RIGHT_HAND_MIDDLE_DISTAL_FB] = BodyJoint::RightHandMiddleDistal;
	result[XR_BODY_JOINT_RIGHT_HAND_MIDDLE_TIP_FB] = BodyJoint::RightHandMiddleTip;
	result[XR_BODY_JOINT_RIGHT_HAND_RING_METACARPAL_FB] = BodyJoint::RightHandRingMetacarpal;
	result[XR_BODY_JOINT_RIGHT_HAND_RING_PROXIMAL_FB] = BodyJoint::RightHandRingProximal;
	result[XR_BODY_JOINT_RIGHT_HAND_RING_INTERMEDIATE_FB] = BodyJoint::RightHandRingIntermediate;
	result[XR_BODY_JOINT_RIGHT_HAND_RING_DISTAL_FB] = BodyJoint::RightHandRingDistal;
	result[XR_BODY_JOINT_RIGHT_HAND_RING_TIP_FB] = BodyJoint::RightHandRingTip;
	result[XR_BODY_JOINT_RIGHT_HAND_LITTLE_METACARPAL_FB] = BodyJoint::RightHandLittleMetacarpal;
	result[XR_BODY_JOINT_RIGHT_HAND_LITTLE_PROXIMAL_FB] = BodyJoint::RightHandLittleProximal;
	result[XR_BODY_JOINT_RIGHT_HAND_LITTLE_INTERMEDIATE_FB] = BodyJoint::RightHandLittleIntermediate;
	result[XR_BODY_JOINT_RIGHT_HAND_LITTLE_DISTAL_FB] = BodyJoint::RightHandLittleDistal;
	result[XR_BODY_JOINT_RIGHT_HAND_LITTLE_TIP_FB] = BodyJoint::RightHandLittleTip;
	return result;
}();

const std::array<BodyJoint, XR_FULL_BODY_JOINT_COUNT_META> s_jointsMETA = []
{
	std::array<BodyJoint, XR_FULL_BODY_JOINT_COUNT_META> result{};
	result[XR_FULL_BODY_JOINT_ROOT_META] = BodyJoint::Root;
	result[XR_FULL_BODY_JOINT_HIPS_META] = BodyJoint::Hips;
	result[XR_FULL_BODY_JOINT_SPINE_LOWER_META] = BodyJoint::SpineLower;
	result[XR_FULL_BODY_JOINT_SPINE_MIDDLE_META] = BodyJoint::SpineMiddle;
	result[XR_FULL_BODY_JOINT_SPINE_UPPER_META] = BodyJoint::SpineUpper;
	result[XR_FULL_BODY_JOINT_CHEST_META] = BodyJoint::Chest;
	result[XR_FULL_BODY_JOINT_NECK_META] = BodyJoint::Neck;
	result[XR_FULL_BODY_JOINT_HEAD_META] = BodyJoint::Head;
	result[XR_FULL_BODY_JOINT_LEFT_SHOULDER_META] = BodyJoint::LeftShoulder;
	result[XR_FULL_BODY_JOINT_LEFT_SCAPULA_META] = BodyJoint::LeftScapula;
	result[XR_FULL_BODY_JOINT_LEFT_ARM_UPPER_META] = BodyJoint::LeftArmUpper;
	result[XR_FULL_BODY_JOINT_LEFT_ARM_LOWER_META] = BodyJoint::LeftArmLower;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_WRIST_TWIST_META] = BodyJoint::LeftHandWristTwist;
	result[XR_FULL_BODY_JOINT_RIGHT_SHOULDER_META] = BodyJoint::RightShoulder;
	result[XR_FULL_BODY_JOINT_RIGHT_SCAPULA_META] = BodyJoint::RightScapula;
	result[XR_FULL_BODY_JOINT_RIGHT_ARM_UPPER_META] = BodyJoint::RightArmUpper;
	result[XR_FULL_BODY_JOINT_RIGHT_ARM_LOWER_META] = BodyJoint::RightArmLower;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_WRIST_TWIST_META] = BodyJoint::RightHandWristTwist;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_PALM_META] = BodyJoint::LeftHandPalm;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_WRIST_META] = BodyJoint::LeftHandWrist;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_THUMB_METACARPAL_META] = BodyJoint::LeftHandThumbMetacarpal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_THUMB_PROXIMAL_META] = BodyJoint::LeftHandThumbProximal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_THUMB_DISTAL_META] = BodyJoint::LeftHandThumbDistal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_THUMB_TIP_META] = BodyJoint::LeftHandThumbTip;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_INDEX_METACARPAL_META] = BodyJoint::LeftHandIndexMetacarpal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_INDEX_PROXIMAL_META] = BodyJoint::LeftHandIndexProximal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_INDEX_INTERMEDIATE_META] = BodyJoint::LeftHandIndexIntermediate;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_INDEX_DISTAL_META] = BodyJoint::LeftHandIndexDistal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_INDEX_TIP_META] = BodyJoint::LeftHandIndexTip;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_MIDDLE_METACARPAL_META] = BodyJoint::LeftHandMiddleMetacarpal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_MIDDLE_PROXIMAL_META] = BodyJoint::LeftHandMiddleProximal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_MIDDLE_INTERMEDIATE_META] = BodyJoint::LeftHandMiddleIntermediate;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_MIDDLE_DISTAL_META] = BodyJoint::LeftHandMiddleDistal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_MIDDLE_TIP_META] = BodyJoint::LeftHandMiddleTip;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_RING_METACARPAL_META] = BodyJoint::LeftHandRingMetacarpal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_RING_PROXIMAL_META] = BodyJoint::LeftHandRingProximal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_RING_INTERMEDIATE_META] = BodyJoint::LeftHandRingIntermediate;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_RING_DISTAL_META] = BodyJoint::LeftHandRingDistal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_RING_TIP_META] = BodyJoint::LeftHandRingTip;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_LITTLE_METACARPAL_META] = BodyJoint::LeftHandLittleMetacarpal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_LITTLE_PROXIMAL_META] = BodyJoint::LeftHandLittleProximal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_LITTLE_INTERMEDIATE_META] = BodyJoint::LeftHandLittleIntermediate;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_LITTLE_DISTAL_META] = BodyJoint::LeftHandLittleDistal;
	result[XR_FULL_BODY_JOINT_LEFT_HAND_LITTLE_TIP_META] = BodyJoint::LeftHandLittleTip;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_PALM_META] = BodyJoint::RightHandPalm;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_WRIST_META] = BodyJoint::RightHandWrist;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_THUMB_METACARPAL_META] = BodyJoint::RightHandThumbMetacarpal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_THUMB_PROXIMAL_META] = BodyJoint::RightHandThumbProximal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_THUMB_DISTAL_META] = BodyJoint::RightHandThumbDistal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_THUMB_TIP_META] = BodyJoint::RightHandThumbTip;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_INDEX_METACARPAL_META] = BodyJoint::RightHandIndexMetacarpal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_INDEX_PROXIMAL_META] = BodyJoint::RightHandIndexProximal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_INDEX_INTERMEDIATE_META] = BodyJoint::RightHandIndexIntermediate;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_INDEX_DISTAL_META] = BodyJoint::RightHandIndexDistal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_INDEX_TIP_META] = BodyJoint::RightHandIndexTip;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_MIDDLE_METACARPAL_META] = BodyJoint::RightHandMiddleMetacarpal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_MIDDLE_PROXIMAL_META] = BodyJoint::RightHandMiddleProximal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_MIDDLE_INTERMEDIATE_META] = BodyJoint::RightHandMiddleIntermediate;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_MIDDLE_DISTAL_META] = BodyJoint::RightHandMiddleDistal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_MIDDLE_TIP_META] = BodyJoint::RightHandMiddleTip;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_RING_METACARPAL_META] = BodyJoint::RightHandRingMetacarpal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_RING_PROXIMAL_META] = BodyJoint::RightHandRingProximal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_RING_INTERMEDIATE_META] = BodyJoint::RightHandRingIntermediate;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_RING_DISTAL_META] = BodyJoint::RightHandRingDistal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_RING_TIP_META] = BodyJoint::RightHandRingTip;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_LITTLE_METACARPAL_META] = BodyJoint::RightHandLittleMetacarpal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_LITTLE_PROXIMAL_META] = BodyJoint::RightHandLittleProximal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_LITTLE_INTERMEDIATE_META] = BodyJoint::RightHandLittleIntermediate;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_LITTLE_DISTAL_META] = BodyJoint::RightHandLittleDistal;
	result[XR_FULL_BODY_JOINT_RIGHT_HAND_LITTLE_TIP_META] = BodyJoint::RightHandLittleTip;
	result[XR_FULL_BODY_JOINT_LEFT_UPPER_LEG_META] = BodyJoint::LeftUpperLeg;
	result[XR_FULL_BODY_JOINT_LEFT_LOWER_LEG_META] = BodyJoint::LeftLowerLeg;
	result[XR_FULL_BODY_JOINT_LEFT_FOOT_ANKLE_TWIST_META] = BodyJoint::LeftFootAnkleTwist;
	result[XR_FULL_BODY_JOINT_LEFT_FOOT_ANKLE_META] = BodyJoint::LeftFootAnkle;
	result[XR_FULL_BODY_JOINT_LEFT_FOOT_SUBTALAR_META] = BodyJoint::LeftFootSubtalar;
	result[XR_FULL_BODY_JOINT_LEFT_FOOT_TRANSVERSE_META] = BodyJoint::LeftFootTransverse;
	result[XR_FULL_BODY_JOINT_LEFT_FOOT_BALL_META] = BodyJoint::LeftFootBall;
	result[XR_FULL_BODY_JOINT_RIGHT_UPPER_LEG_META] = BodyJoint::RightUpperLeg;
	result[XR_FULL_BODY_JOINT_RIGHT_LOWER_LEG_META] = BodyJoint::RightLowerLeg;
	result[XR_FULL_BODY_JOINT_RIGHT_FOOT_ANKLE_TWIST_META] = BodyJoint::RightFootAnkleTwist;
	result[XR_FULL_BODY_JOINT_RIGHT_FOOT_ANKLE_META] = BodyJoint::RightFootAnkle;
	result[XR_FULL_BODY_JOINT_RIGHT_FOOT_SUBTALAR_META] = BodyJoint::RightFootSubtalar;
	result[XR_FULL_BODY_JOINT_RIGHT_FOOT_TRANSVERSE_META] = BodyJoint::RightFootTransverse;
	result[XR_FULL_BODY_JOINT_RIGHT_FOOT_BALL_META] = BodyJoint::RightFootBall;
	return result;
}();

const std::array<BodyJoint, XR_BODY_JOINT_COUNT_BD> s_jointsBD = []
{
	std::array<BodyJoint, XR_BODY_JOINT_COUNT_BD> result{};
	result[XR_BODY_JOINT_PELVIS_BD] = BodyJoint::Hips;
	result[XR_BODY_JOINT_LEFT_HIP_BD] = BodyJoint::LeftUpperLeg;
	result[XR_BODY_JOINT_RIGHT_HIP_BD] = BodyJoint::RightUpperLeg;
	result[XR_BODY_JOINT_SPINE1_BD] = BodyJoint::SpineLower;
	result[XR_BODY_JOINT_LEFT_KNEE_BD] = BodyJoint::LeftLowerLeg;
	result[XR_BODY_JOINT_RIGHT_KNEE_BD] = BodyJoint::RightLowerLeg;
	result[XR_BODY_JOINT_SPINE2_BD] = BodyJoint::SpineMiddle;
	result[XR_BODY_JOINT_LEFT_ANKLE_BD] = BodyJoint::LeftFootAnkle;
	result[XR_BODY_JOINT_RIGHT_ANKLE_BD] = BodyJoint::RightFootAnkle;
	result[XR_BODY_JOINT_SPINE3_BD] = BodyJoint::SpineUpper;
	result[XR_BODY_JOINT_LEFT_FOOT_BD] = BodyJoint::LeftFootBall;
	result[XR_BODY_JOINT_RIGHT_FOOT_BD] = BodyJoint::RightFootBall;
	result[XR_BODY_JOINT_NECK_BD] = BodyJoint::Neck;
	result[XR_BODY_JOINT_LEFT_COLLAR_BD] = BodyJoint::LeftScapula;
	result[XR_BODY_JOINT_RIGHT_COLLAR_BD] = BodyJoint::RightScapula;
	result[XR_BODY_JOINT_HEAD_BD] = BodyJoint::Head;
	result[XR_BODY_JOINT_LEFT_SHOULDER_BD] = BodyJoint::LeftArmUpper;
	result[XR_BODY_JOINT_RIGHT_SHOULDER_BD] = BodyJoint::RightArmUpper;
	result[XR_BODY_JOINT_LEFT_ELBOW_BD] = BodyJoint::LeftArmLower;
	result[XR_BODY_JOINT_RIGHT_ELBOW_BD] = BodyJoint::RightArmLower;
	result[XR_BODY_JOINT_LEFT_WRIST_BD] = BodyJoint::LeftHandWrist;
	result[XR_BODY_JOINT_RIGHT_WRIST_BD] = BodyJoint::RightHandWrist;
	result[XR_BODY_JOINT_LEFT_HAND_BD] = BodyJoint::LeftHandPalm;
	result[XR_BODY_JOINT_RIGHT_HAND_BD] = BodyJoint::RightHandPalm;
	return result;
}();

XrVector3f Difference(XrVector3f a, XrVector3f b)
{
	return {a.x - b.x, a.y - b.y, a.z - b.z};
}

XrVector3f Blend(XrVector3f a, XrVector3f b, float weight)
{
	return {std::lerp(a.x, b.x, weight), std::lerp(a.y, b.y, weight), std::lerp(a.z, b.z, weight)};
}
}

BodyModel::BodyModel(const BodyProportions& proportions)
{
	SetProportions(proportions);
}

void BodyModel::SetProportions(const BodyProportions& proportions)
{
	if (!std::isfinite(proportions.height) || !std::isfinite(proportions.shoulderWidth) ||
		!std::isfinite(proportions.hipWidth) || proportions.height <= 0 ||
		proportions.shoulderWidth <= 0 || proportions.hipWidth <= 0)
		throw std::invalid_argument("Invalid body proportions");
	if (m_revision != 0 && proportions == m_proportions)
		return;
	if (m_revision == std::numeric_limits<std::uint32_t>::max())
		throw std::overflow_error("Skeleton revisions exhausted");
	m_proportions = proportions;
	++m_revision;
	Rebuild();
}

const BodyProportions& BodyModel::Proportions() const
{
	return m_proportions;
}

std::uint32_t BodyModel::Revision() const
{
	return m_revision;
}

std::span<const SkeletonJoint> BodyModel::Skeleton() const
{
	return m_skeleton;
}

std::span<const BodyJoint> BodyModel::JointMap(BodyJointSet set)
{
	switch (set)
	{
	case BodyJointSet::UpperBodyFB: return s_jointsFB;
	case BodyJointSet::FullBodyMETA: return s_jointsMETA;
	case BodyJointSet::WithoutArmsBD: return std::span(s_jointsBD).first(XR_BODY_JOINT_WITHOUT_ARM_COUNT_BD);
	case BodyJointSet::FullBodyBD: return s_jointsBD;
	}
	throw std::invalid_argument("Unknown body joint set");
}

BodyJoint BodyModel::TrackerJoint(TrackerRole role)
{
	switch (role)
	{
	case TrackerRole::Waist: return BodyJoint::Hips;
	case TrackerRole::Chest: return BodyJoint::Chest;
	case TrackerRole::LeftFoot: return BodyJoint::LeftFootAnkle;
	case TrackerRole::RightFoot: return BodyJoint::RightFootAnkle;
	case TrackerRole::Unassigned: return BodyJoint::Hips;
	case TrackerRole::HandheldObject: return BodyJoint::RightHandPalm;
	case TrackerRole::LeftShoulder: return BodyJoint::LeftShoulder;
	case TrackerRole::RightShoulder: return BodyJoint::RightShoulder;
	case TrackerRole::LeftElbow: return BodyJoint::LeftArmLower;
	case TrackerRole::RightElbow: return BodyJoint::RightArmLower;
	case TrackerRole::LeftKnee: return BodyJoint::LeftLowerLeg;
	case TrackerRole::RightKnee: return BodyJoint::RightLowerLeg;
	case TrackerRole::LeftWrist: return BodyJoint::LeftHandWrist;
	case TrackerRole::RightWrist: return BodyJoint::RightHandWrist;
	case TrackerRole::LeftAnkle: return BodyJoint::LeftFootAnkle;
	case TrackerRole::RightAnkle: return BodyJoint::RightFootAnkle;
	case TrackerRole::Camera: return BodyJoint::Head;
	case TrackerRole::Keyboard: return BodyJoint::Hips;
	}
	throw std::invalid_argument("Unknown tracker role");
}

std::string_view BodyModel::TrackerRolePath(TrackerRole role)
{
	switch (role)
	{
	case TrackerRole::Waist: return "/user/vive_tracker_htcx/role/waist";
	case TrackerRole::Chest: return "/user/vive_tracker_htcx/role/chest";
	case TrackerRole::LeftFoot: return "/user/vive_tracker_htcx/role/left_foot";
	case TrackerRole::RightFoot: return "/user/vive_tracker_htcx/role/right_foot";
	case TrackerRole::Unassigned: return {};
	case TrackerRole::HandheldObject: return "/user/vive_tracker_htcx/role/handheld_object";
	case TrackerRole::LeftShoulder: return "/user/vive_tracker_htcx/role/left_shoulder";
	case TrackerRole::RightShoulder: return "/user/vive_tracker_htcx/role/right_shoulder";
	case TrackerRole::LeftElbow: return "/user/vive_tracker_htcx/role/left_elbow";
	case TrackerRole::RightElbow: return "/user/vive_tracker_htcx/role/right_elbow";
	case TrackerRole::LeftKnee: return "/user/vive_tracker_htcx/role/left_knee";
	case TrackerRole::RightKnee: return "/user/vive_tracker_htcx/role/right_knee";
	case TrackerRole::LeftWrist: return "/user/vive_tracker_htcx/role/left_wrist";
	case TrackerRole::RightWrist: return "/user/vive_tracker_htcx/role/right_wrist";
	case TrackerRole::LeftAnkle: return "/user/vive_tracker_htcx/role/left_ankle";
	case TrackerRole::RightAnkle: return "/user/vive_tracker_htcx/role/right_ankle";
	case TrackerRole::Camera: return "/user/vive_tracker_htcx/role/camera";
	case TrackerRole::Keyboard: return "/user/vive_tracker_htcx/role/keyboard";
	}
	throw std::invalid_argument("Unknown tracker role");
}

std::string BodyModel::TrackerPersistentPath(TrackerIdentity identity)
{
	if (!identity.value)
		throw std::invalid_argument("Missing persistent tracker identity");
	char path[80]{};
	std::snprintf(path, sizeof(path), "/user/vive_tracker_htcx/serial/simxr_%016llx",
		static_cast<unsigned long long>(identity.value));
	return path;
}

void BodyModel::Rebuild()
{
	const auto height = m_proportions.height;
	const auto shoulder = m_proportions.shoulderWidth / 2;
	const auto hip = m_proportions.hipWidth / 2;
	auto set = [&](BodyJoint joint, std::optional<BodyJoint> parent, XrVector3f position)
	{
		m_skeleton[Index(joint)] = {joint, parent, {{0, 0, 0, 1}, position}};
	};
	set(BodyJoint::Root, {}, {0, 0, 0});
	set(BodyJoint::Hips, BodyJoint::Root, {0, height * 0.53f, 0.05f});
	set(BodyJoint::SpineLower, BodyJoint::Hips, {0, height * 0.59f, 0.045f});
	set(BodyJoint::SpineMiddle, BodyJoint::SpineLower, {0, height * 0.66f, 0.04f});
	set(BodyJoint::SpineUpper, BodyJoint::SpineMiddle, {0, height * 0.71f, 0.04f});
	set(BodyJoint::Chest, BodyJoint::SpineUpper, {0, height * 0.76f, 0.04f});
	set(BodyJoint::Neck, BodyJoint::Chest, {0, height * 0.9f, 0.01f});
	set(BodyJoint::Head, BodyJoint::Neck, {0, height, 0});
	set(BodyJoint::LeftShoulder, BodyJoint::Chest, {-1 * shoulder, height * 0.82f, 0.02f});
	set(BodyJoint::LeftScapula, BodyJoint::LeftShoulder, {-1 * shoulder * 0.8f, height * 0.79f, 0.06f});
	set(BodyJoint::LeftArmUpper, BodyJoint::LeftScapula, {-1 * (shoulder + 0.04f), height * 0.78f, 0});
	set(BodyJoint::LeftArmLower, BodyJoint::LeftArmUpper, {-1 * (shoulder + 0.11f), height * 0.64f, -0.04f});
	set(BodyJoint::LeftHandWristTwist, BodyJoint::LeftArmLower, {-1 * (shoulder + 0.14f), height * 0.56f, -0.065f});
	set(BodyJoint::LeftHandWrist, BodyJoint::LeftHandWristTwist, {-1 * (shoulder + 0.145f), height * 0.54f, -0.07f});
	set(BodyJoint::LeftHandPalm, BodyJoint::LeftHandWrist, {-1 * (shoulder + 0.15f), height * 0.51f, -0.07f});
	set(BodyJoint::LeftHandThumbMetacarpal, BodyJoint::LeftHandPalm, {-1 * (shoulder + 0.12f + 0 * 0.015f), height * 0.51f - (0 + 1) * 0.019f, -0.09f - 0 * 0.008f});
	set(BodyJoint::LeftHandThumbProximal, BodyJoint::LeftHandThumbMetacarpal, {-1 * (shoulder + 0.12f + 0 * 0.015f), height * 0.51f - (1 + 1) * 0.019f, -0.09f - 0 * 0.008f});
	set(BodyJoint::LeftHandThumbDistal, BodyJoint::LeftHandThumbProximal, {-1 * (shoulder + 0.12f + 0 * 0.015f), height * 0.51f - (2 + 1) * 0.019f, -0.09f - 0 * 0.008f});
	set(BodyJoint::LeftHandThumbTip, BodyJoint::LeftHandThumbDistal, {-1 * (shoulder + 0.12f + 0 * 0.015f), height * 0.51f - (3 + 1) * 0.019f, -0.09f - 0 * 0.008f});
	set(BodyJoint::LeftHandIndexMetacarpal, BodyJoint::LeftHandPalm, {-1 * (shoulder + 0.12f + 1 * 0.015f), height * 0.51f - (0 + 1) * 0.019f, -0.09f - 1 * 0.008f});
	set(BodyJoint::LeftHandIndexProximal, BodyJoint::LeftHandIndexMetacarpal, {-1 * (shoulder + 0.12f + 1 * 0.015f), height * 0.51f - (1 + 1) * 0.019f, -0.09f - 1 * 0.008f});
	set(BodyJoint::LeftHandIndexIntermediate, BodyJoint::LeftHandIndexProximal, {-1 * (shoulder + 0.12f + 1 * 0.015f), height * 0.51f - (2 + 1) * 0.019f, -0.09f - 1 * 0.008f});
	set(BodyJoint::LeftHandIndexDistal, BodyJoint::LeftHandIndexIntermediate, {-1 * (shoulder + 0.12f + 1 * 0.015f), height * 0.51f - (3 + 1) * 0.019f, -0.09f - 1 * 0.008f});
	set(BodyJoint::LeftHandIndexTip, BodyJoint::LeftHandIndexDistal, {-1 * (shoulder + 0.12f + 1 * 0.015f), height * 0.51f - (4 + 1) * 0.019f, -0.09f - 1 * 0.008f});
	set(BodyJoint::LeftHandMiddleMetacarpal, BodyJoint::LeftHandPalm, {-1 * (shoulder + 0.12f + 2 * 0.015f), height * 0.51f - (0 + 1) * 0.019f, -0.09f - 2 * 0.008f});
	set(BodyJoint::LeftHandMiddleProximal, BodyJoint::LeftHandMiddleMetacarpal, {-1 * (shoulder + 0.12f + 2 * 0.015f), height * 0.51f - (1 + 1) * 0.019f, -0.09f - 2 * 0.008f});
	set(BodyJoint::LeftHandMiddleIntermediate, BodyJoint::LeftHandMiddleProximal, {-1 * (shoulder + 0.12f + 2 * 0.015f), height * 0.51f - (2 + 1) * 0.019f, -0.09f - 2 * 0.008f});
	set(BodyJoint::LeftHandMiddleDistal, BodyJoint::LeftHandMiddleIntermediate, {-1 * (shoulder + 0.12f + 2 * 0.015f), height * 0.51f - (3 + 1) * 0.019f, -0.09f - 2 * 0.008f});
	set(BodyJoint::LeftHandMiddleTip, BodyJoint::LeftHandMiddleDistal, {-1 * (shoulder + 0.12f + 2 * 0.015f), height * 0.51f - (4 + 1) * 0.019f, -0.09f - 2 * 0.008f});
	set(BodyJoint::LeftHandRingMetacarpal, BodyJoint::LeftHandPalm, {-1 * (shoulder + 0.12f + 3 * 0.015f), height * 0.51f - (0 + 1) * 0.019f, -0.09f - 3 * 0.008f});
	set(BodyJoint::LeftHandRingProximal, BodyJoint::LeftHandRingMetacarpal, {-1 * (shoulder + 0.12f + 3 * 0.015f), height * 0.51f - (1 + 1) * 0.019f, -0.09f - 3 * 0.008f});
	set(BodyJoint::LeftHandRingIntermediate, BodyJoint::LeftHandRingProximal, {-1 * (shoulder + 0.12f + 3 * 0.015f), height * 0.51f - (2 + 1) * 0.019f, -0.09f - 3 * 0.008f});
	set(BodyJoint::LeftHandRingDistal, BodyJoint::LeftHandRingIntermediate, {-1 * (shoulder + 0.12f + 3 * 0.015f), height * 0.51f - (3 + 1) * 0.019f, -0.09f - 3 * 0.008f});
	set(BodyJoint::LeftHandRingTip, BodyJoint::LeftHandRingDistal, {-1 * (shoulder + 0.12f + 3 * 0.015f), height * 0.51f - (4 + 1) * 0.019f, -0.09f - 3 * 0.008f});
	set(BodyJoint::LeftHandLittleMetacarpal, BodyJoint::LeftHandPalm, {-1 * (shoulder + 0.12f + 4 * 0.015f), height * 0.51f - (0 + 1) * 0.019f, -0.09f - 4 * 0.008f});
	set(BodyJoint::LeftHandLittleProximal, BodyJoint::LeftHandLittleMetacarpal, {-1 * (shoulder + 0.12f + 4 * 0.015f), height * 0.51f - (1 + 1) * 0.019f, -0.09f - 4 * 0.008f});
	set(BodyJoint::LeftHandLittleIntermediate, BodyJoint::LeftHandLittleProximal, {-1 * (shoulder + 0.12f + 4 * 0.015f), height * 0.51f - (2 + 1) * 0.019f, -0.09f - 4 * 0.008f});
	set(BodyJoint::LeftHandLittleDistal, BodyJoint::LeftHandLittleIntermediate, {-1 * (shoulder + 0.12f + 4 * 0.015f), height * 0.51f - (3 + 1) * 0.019f, -0.09f - 4 * 0.008f});
	set(BodyJoint::LeftHandLittleTip, BodyJoint::LeftHandLittleDistal, {-1 * (shoulder + 0.12f + 4 * 0.015f), height * 0.51f - (4 + 1) * 0.019f, -0.09f - 4 * 0.008f});
	set(BodyJoint::LeftUpperLeg, BodyJoint::Hips, {-1 * hip, height * 0.51f, 0.04f});
	set(BodyJoint::LeftLowerLeg, BodyJoint::LeftUpperLeg, {-1 * hip, height * 0.28f, 0.01f});
	set(BodyJoint::LeftFootAnkleTwist, BodyJoint::LeftLowerLeg, {-1 * hip, 0.12f, 0.02f});
	set(BodyJoint::LeftFootAnkle, BodyJoint::LeftFootAnkleTwist, {-1 * hip, 0.08f, 0.02f});
	set(BodyJoint::LeftFootSubtalar, BodyJoint::LeftFootAnkle, {-1 * hip, 0.05f, 0.01f});
	set(BodyJoint::LeftFootTransverse, BodyJoint::LeftFootSubtalar, {-1 * hip, 0.035f, -0.07f});
	set(BodyJoint::LeftFootBall, BodyJoint::LeftFootTransverse, {-1 * hip, 0.025f, -0.13f});
	set(BodyJoint::RightShoulder, BodyJoint::Chest, {1 * shoulder, height * 0.82f, 0.02f});
	set(BodyJoint::RightScapula, BodyJoint::RightShoulder, {1 * shoulder * 0.8f, height * 0.79f, 0.06f});
	set(BodyJoint::RightArmUpper, BodyJoint::RightScapula, {1 * (shoulder + 0.04f), height * 0.78f, 0});
	set(BodyJoint::RightArmLower, BodyJoint::RightArmUpper, {1 * (shoulder + 0.11f), height * 0.64f, -0.04f});
	set(BodyJoint::RightHandWristTwist, BodyJoint::RightArmLower, {1 * (shoulder + 0.14f), height * 0.56f, -0.065f});
	set(BodyJoint::RightHandWrist, BodyJoint::RightHandWristTwist, {1 * (shoulder + 0.145f), height * 0.54f, -0.07f});
	set(BodyJoint::RightHandPalm, BodyJoint::RightHandWrist, {1 * (shoulder + 0.15f), height * 0.51f, -0.07f});
	set(BodyJoint::RightHandThumbMetacarpal, BodyJoint::RightHandPalm, {1 * (shoulder + 0.12f + 0 * 0.015f), height * 0.51f - (0 + 1) * 0.019f, -0.09f - 0 * 0.008f});
	set(BodyJoint::RightHandThumbProximal, BodyJoint::RightHandThumbMetacarpal, {1 * (shoulder + 0.12f + 0 * 0.015f), height * 0.51f - (1 + 1) * 0.019f, -0.09f - 0 * 0.008f});
	set(BodyJoint::RightHandThumbDistal, BodyJoint::RightHandThumbProximal, {1 * (shoulder + 0.12f + 0 * 0.015f), height * 0.51f - (2 + 1) * 0.019f, -0.09f - 0 * 0.008f});
	set(BodyJoint::RightHandThumbTip, BodyJoint::RightHandThumbDistal, {1 * (shoulder + 0.12f + 0 * 0.015f), height * 0.51f - (3 + 1) * 0.019f, -0.09f - 0 * 0.008f});
	set(BodyJoint::RightHandIndexMetacarpal, BodyJoint::RightHandPalm, {1 * (shoulder + 0.12f + 1 * 0.015f), height * 0.51f - (0 + 1) * 0.019f, -0.09f - 1 * 0.008f});
	set(BodyJoint::RightHandIndexProximal, BodyJoint::RightHandIndexMetacarpal, {1 * (shoulder + 0.12f + 1 * 0.015f), height * 0.51f - (1 + 1) * 0.019f, -0.09f - 1 * 0.008f});
	set(BodyJoint::RightHandIndexIntermediate, BodyJoint::RightHandIndexProximal, {1 * (shoulder + 0.12f + 1 * 0.015f), height * 0.51f - (2 + 1) * 0.019f, -0.09f - 1 * 0.008f});
	set(BodyJoint::RightHandIndexDistal, BodyJoint::RightHandIndexIntermediate, {1 * (shoulder + 0.12f + 1 * 0.015f), height * 0.51f - (3 + 1) * 0.019f, -0.09f - 1 * 0.008f});
	set(BodyJoint::RightHandIndexTip, BodyJoint::RightHandIndexDistal, {1 * (shoulder + 0.12f + 1 * 0.015f), height * 0.51f - (4 + 1) * 0.019f, -0.09f - 1 * 0.008f});
	set(BodyJoint::RightHandMiddleMetacarpal, BodyJoint::RightHandPalm, {1 * (shoulder + 0.12f + 2 * 0.015f), height * 0.51f - (0 + 1) * 0.019f, -0.09f - 2 * 0.008f});
	set(BodyJoint::RightHandMiddleProximal, BodyJoint::RightHandMiddleMetacarpal, {1 * (shoulder + 0.12f + 2 * 0.015f), height * 0.51f - (1 + 1) * 0.019f, -0.09f - 2 * 0.008f});
	set(BodyJoint::RightHandMiddleIntermediate, BodyJoint::RightHandMiddleProximal, {1 * (shoulder + 0.12f + 2 * 0.015f), height * 0.51f - (2 + 1) * 0.019f, -0.09f - 2 * 0.008f});
	set(BodyJoint::RightHandMiddleDistal, BodyJoint::RightHandMiddleIntermediate, {1 * (shoulder + 0.12f + 2 * 0.015f), height * 0.51f - (3 + 1) * 0.019f, -0.09f - 2 * 0.008f});
	set(BodyJoint::RightHandMiddleTip, BodyJoint::RightHandMiddleDistal, {1 * (shoulder + 0.12f + 2 * 0.015f), height * 0.51f - (4 + 1) * 0.019f, -0.09f - 2 * 0.008f});
	set(BodyJoint::RightHandRingMetacarpal, BodyJoint::RightHandPalm, {1 * (shoulder + 0.12f + 3 * 0.015f), height * 0.51f - (0 + 1) * 0.019f, -0.09f - 3 * 0.008f});
	set(BodyJoint::RightHandRingProximal, BodyJoint::RightHandRingMetacarpal, {1 * (shoulder + 0.12f + 3 * 0.015f), height * 0.51f - (1 + 1) * 0.019f, -0.09f - 3 * 0.008f});
	set(BodyJoint::RightHandRingIntermediate, BodyJoint::RightHandRingProximal, {1 * (shoulder + 0.12f + 3 * 0.015f), height * 0.51f - (2 + 1) * 0.019f, -0.09f - 3 * 0.008f});
	set(BodyJoint::RightHandRingDistal, BodyJoint::RightHandRingIntermediate, {1 * (shoulder + 0.12f + 3 * 0.015f), height * 0.51f - (3 + 1) * 0.019f, -0.09f - 3 * 0.008f});
	set(BodyJoint::RightHandRingTip, BodyJoint::RightHandRingDistal, {1 * (shoulder + 0.12f + 3 * 0.015f), height * 0.51f - (4 + 1) * 0.019f, -0.09f - 3 * 0.008f});
	set(BodyJoint::RightHandLittleMetacarpal, BodyJoint::RightHandPalm, {1 * (shoulder + 0.12f + 4 * 0.015f), height * 0.51f - (0 + 1) * 0.019f, -0.09f - 4 * 0.008f});
	set(BodyJoint::RightHandLittleProximal, BodyJoint::RightHandLittleMetacarpal, {1 * (shoulder + 0.12f + 4 * 0.015f), height * 0.51f - (1 + 1) * 0.019f, -0.09f - 4 * 0.008f});
	set(BodyJoint::RightHandLittleIntermediate, BodyJoint::RightHandLittleProximal, {1 * (shoulder + 0.12f + 4 * 0.015f), height * 0.51f - (2 + 1) * 0.019f, -0.09f - 4 * 0.008f});
	set(BodyJoint::RightHandLittleDistal, BodyJoint::RightHandLittleIntermediate, {1 * (shoulder + 0.12f + 4 * 0.015f), height * 0.51f - (3 + 1) * 0.019f, -0.09f - 4 * 0.008f});
	set(BodyJoint::RightHandLittleTip, BodyJoint::RightHandLittleDistal, {1 * (shoulder + 0.12f + 4 * 0.015f), height * 0.51f - (4 + 1) * 0.019f, -0.09f - 4 * 0.008f});
	set(BodyJoint::RightUpperLeg, BodyJoint::Hips, {1 * hip, height * 0.51f, 0.04f});
	set(BodyJoint::RightLowerLeg, BodyJoint::RightUpperLeg, {1 * hip, height * 0.28f, 0.01f});
	set(BodyJoint::RightFootAnkleTwist, BodyJoint::RightLowerLeg, {1 * hip, 0.12f, 0.02f});
	set(BodyJoint::RightFootAnkle, BodyJoint::RightFootAnkleTwist, {1 * hip, 0.08f, 0.02f});
	set(BodyJoint::RightFootSubtalar, BodyJoint::RightFootAnkle, {1 * hip, 0.05f, 0.01f});
	set(BodyJoint::RightFootTransverse, BodyJoint::RightFootSubtalar, {1 * hip, 0.035f, -0.07f});
	set(BodyJoint::RightFootBall, BodyJoint::RightFootTransverse, {1 * hip, 0.025f, -0.13f});
}

BodySample BodyModel::Evaluate(const PoseSample& inputHead, const BodyControls& controls, RuntimeTime time, bool focused) const
{
	if (!std::isfinite(controls.confidence) || controls.confidence < 0 || controls.confidence > 1 ||
		(controls.preset != BodyPreset::Standing && controls.preset != BodyPreset::TPose && controls.preset != BodyPreset::Seated))
		throw std::invalid_argument("Invalid body controls");
	auto head = inputHead;
	auto& headOrientation = head.pose.orientation;
	float headLength = std::sqrt(headOrientation.x * headOrientation.x + headOrientation.y * headOrientation.y + headOrientation.z * headOrientation.z + headOrientation.w * headOrientation.w);
	if (time.value.count() <= 0 || !IsFinite(head.pose.position) || !std::isfinite(headLength) ||
		std::abs(headLength - 1) > 0.01f)
		throw std::invalid_argument("Invalid body head pose or time");
	headOrientation = {headOrientation.x / headLength, headOrientation.y / headLength, headOrientation.z / headLength, headOrientation.w / headLength};
	for (const auto& override : controls.joints)
	{
		if (!override || !override->pose)
			continue;
		const auto& pose = *override->pose;
		const auto& q = pose.orientation;
		float norm = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
		if (!IsFinite(pose.position) || !std::isfinite(norm) || std::abs(norm - 1) > 0.01f)
			throw std::invalid_argument("Invalid joint override");
	}
	BodySample sample;
	sample.active = controls.active && focused && (head.flags & ValidPoseFlags) == ValidPoseFlags;
	sample.confidence = sample.active ? controls.confidence : 0;
	sample.skeletonRevision = m_revision;
	sample.sampleTime = time;
	sample.skeleton = m_skeleton;
	auto forward = Rotate(head.pose.orientation, {0, 0, -1});
	float yaw = std::atan2(-forward.x, -forward.z);
	auto orientation = EulerAngles(yaw, 0, 0);
	XrPosef root{orientation, {head.pose.position.x, 0, head.pose.position.z}};
	if (controls.rootPose)
	{
		const auto normalized = NormalizedPose(*controls.rootPose);
		if (!normalized) throw std::invalid_argument("Invalid body root pose");
		root = *normalized;
	}
	const float headHeight = head.pose.position.y - root.position.y;
	std::array<XrVector3f, 5> defaults{{
		{0, headHeight * 0.53f, 0.05f}, {0, headHeight * 0.76f, 0.04f},
		{0, headHeight, 0}, {-m_proportions.hipWidth / 2, 0.08f, 0.02f}, {m_proportions.hipWidth / 2, 0.08f, 0.02f}}};
	std::array<XrVector3f, 5> shifts{};
	for (std::size_t index = 0; index < shifts.size(); ++index)
	{
		auto offset = controls.controls[index].offset.value_or(defaults[index]);
		if (!IsFinite(offset))
			throw std::invalid_argument("Invalid body control offset");
		shifts[index] = Difference(offset, defaults[index]);
	}
	for (const auto& joint : m_skeleton)
	{
		auto name = joint.joint;
		auto position = joint.bindPose.position;
		position.y *= headHeight / m_proportions.height;
		std::size_t control = static_cast<std::size_t>(BodyControl::Chest);
		bool leftLeg = name == BodyJoint::LeftUpperLeg || name == BodyJoint::LeftLowerLeg || name == BodyJoint::LeftFootAnkleTwist ||
			name == BodyJoint::LeftFootAnkle || name == BodyJoint::LeftFootSubtalar || name == BodyJoint::LeftFootTransverse || name == BodyJoint::LeftFootBall;
		bool rightLeg = name == BodyJoint::RightUpperLeg || name == BodyJoint::RightLowerLeg || name == BodyJoint::RightFootAnkleTwist ||
			name == BodyJoint::RightFootAnkle || name == BodyJoint::RightFootSubtalar || name == BodyJoint::RightFootTransverse || name == BodyJoint::RightFootBall;
		XrVector3f shift;
		if (leftLeg || rightLeg)
		{
			control = static_cast<std::size_t>(leftLeg ? BodyControl::LeftFoot : BodyControl::RightFoot);
			float weight = (name == BodyJoint::LeftUpperLeg || name == BodyJoint::RightUpperLeg) ? 0.1f :
				((name == BodyJoint::LeftLowerLeg || name == BodyJoint::RightLowerLeg) ? 0.5f : 1.0f);
			shift = Blend(shifts[0], shifts[control], weight);
			if (weight == 1)
				position.y = joint.bindPose.position.y;
			if (weight == 0.1f)
				control = static_cast<std::size_t>(BodyControl::Hips);
			if (controls.preset == BodyPreset::Seated)
				position.z -= weight * 0.25f;
		}
		else if (name == BodyJoint::Root || name == BodyJoint::Hips || name == BodyJoint::SpineLower)
		{
			control = 0;
			shift = shifts[0];
		}
		else if (name == BodyJoint::Head || name == BodyJoint::Neck)
		{
			control = 2;
			shift = name == BodyJoint::Head ? shifts[2] : Blend(shifts[1], shifts[2], 0.6f);
		}
		else
		{
			shift = shifts[1];
			if (name == BodyJoint::SpineMiddle || name == BodyJoint::SpineUpper)
				shift = Blend(shifts[0], shifts[1], name == BodyJoint::SpineMiddle ? 0.5f : 0.8f);
			if (controls.preset == BodyPreset::TPose && std::abs(position.x) > 0.01f)
			{
				float shoulder = m_proportions.shoulderWidth / 2;
				float distance = std::hypot(std::abs(position.x) - shoulder, position.y - headHeight * 0.82f);
				position.x = std::copysign(shoulder + distance, position.x);
				position.y = headHeight * 0.82f;
			}
		}
		if (controls.preset == BodyPreset::Seated && !leftLeg && !rightLeg && name != BodyJoint::Head && name != BodyJoint::Neck)
			position.y -= name == BodyJoint::Hips ? 0.18f : 0.08f;
		position = {position.x + shift.x, position.y + shift.y, position.z + shift.z};
		if (name == BodyJoint::Root)
			position.y = 0;
		auto& location = sample.joints[Index(name)];
		location.sampleTime = time;
		location.pose = Compose(root, {{0, 0, 0, 1}, position});
		if (name == BodyJoint::Head)
		{
			location.pose.orientation = head.pose.orientation;
			if (controls.rootPose)
			{
				const auto offset = Rotate(root.orientation, shifts[2]);
				location.pose.position = {head.pose.position.x + offset.x, head.pose.position.y + offset.y, head.pose.position.z + offset.z};
			}
		}
		location.flags = sample.active ? ValidPoseFlags : 0;
		if (sample.active && controls.controls[control].tracked)
			location.flags |= head.flags & (XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
		if (const auto& override = controls.joints[Index(name)])
		{
			if (override->pose)
			{
				auto pose = *override->pose;
				auto& q = pose.orientation;
				float length = std::sqrt(q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w);
				q = {q.x / length, q.y / length, q.z / length, q.w / length};
				location.pose = Compose(root, pose);
			}
			if (!override->tracked)
				location.flags &= ValidPoseFlags;
		}
		if (!IsFinite(location.pose.position))
			throw std::invalid_argument("Body joint position is not representable");
	}
	return sample;
}
}
