#pragma once

#include "simulation/TrackerSourcePath.h"

#include "Types.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <openxr/openxr.h>
#include <optional>
#include <string_view>
#include <vector>

namespace simxr
{
inline constexpr XrSpaceLocationFlags ValidPoseFlags =
	XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
inline constexpr XrSpaceLocationFlags TrackedPoseFlags =
	ValidPoseFlags | XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;

struct PoseSample
{
	XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
	XrSpaceLocationFlags flags = 0;
	RuntimeTime sampleTime{};
	std::optional<RuntimeTime> eyeGazeSampleTime;
};

// A value snapshot of one space lookup; it does not retain the space handle.
struct SpaceSample
{
	PoseSample worldPose;
	XrReferenceSpaceType referenceType = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
	XrPosef poseOffset{{0, 0, 0, 1}, {0, 0, 0}};
};

struct ControllerPose
{
	PoseSample pose;
	bool connected = true;
	// Absence derives aim from the profile's grip offset; a present invalid sample is tracking loss.
	std::optional<PoseSample> aim;
};

enum class BodyJoint
{
	Chest,
	Head,
	Hips,
	LeftArmLower,
	LeftArmUpper,
	LeftFootAnkle,
	LeftFootAnkleTwist,
	LeftFootBall,
	LeftFootSubtalar,
	LeftFootTransverse,
	LeftHandIndexDistal,
	LeftHandIndexIntermediate,
	LeftHandIndexMetacarpal,
	LeftHandIndexProximal,
	LeftHandIndexTip,
	LeftHandLittleDistal,
	LeftHandLittleIntermediate,
	LeftHandLittleMetacarpal,
	LeftHandLittleProximal,
	LeftHandLittleTip,
	LeftHandMiddleDistal,
	LeftHandMiddleIntermediate,
	LeftHandMiddleMetacarpal,
	LeftHandMiddleProximal,
	LeftHandMiddleTip,
	LeftHandPalm,
	LeftHandRingDistal,
	LeftHandRingIntermediate,
	LeftHandRingMetacarpal,
	LeftHandRingProximal,
	LeftHandRingTip,
	LeftHandThumbDistal,
	LeftHandThumbMetacarpal,
	LeftHandThumbProximal,
	LeftHandThumbTip,
	LeftHandWrist,
	LeftHandWristTwist,
	LeftLowerLeg,
	LeftScapula,
	LeftShoulder,
	LeftUpperLeg,
	Neck,
	RightArmLower,
	RightArmUpper,
	RightFootAnkle,
	RightFootAnkleTwist,
	RightFootBall,
	RightFootSubtalar,
	RightFootTransverse,
	RightHandIndexDistal,
	RightHandIndexIntermediate,
	RightHandIndexMetacarpal,
	RightHandIndexProximal,
	RightHandIndexTip,
	RightHandLittleDistal,
	RightHandLittleIntermediate,
	RightHandLittleMetacarpal,
	RightHandLittleProximal,
	RightHandLittleTip,
	RightHandMiddleDistal,
	RightHandMiddleIntermediate,
	RightHandMiddleMetacarpal,
	RightHandMiddleProximal,
	RightHandMiddleTip,
	RightHandPalm,
	RightHandRingDistal,
	RightHandRingIntermediate,
	RightHandRingMetacarpal,
	RightHandRingProximal,
	RightHandRingTip,
	RightHandThumbDistal,
	RightHandThumbMetacarpal,
	RightHandThumbProximal,
	RightHandThumbTip,
	RightHandWrist,
	RightHandWristTwist,
	RightLowerLeg,
	RightScapula,
	RightShoulder,
	RightUpperLeg,
	Root,
	SpineLower,
	SpineMiddle,
	SpineUpper,
	Count
};

inline constexpr std::size_t BodyJointCount = static_cast<std::size_t>(BodyJoint::Count);

enum class BodyJointSet
{
	UpperBodyFB,
	FullBodyMETA,
	WithoutArmsBD,
	FullBodyBD
};

inline constexpr std::array<std::string_view, 4> BodyJointSetNames{"fb-upper", "meta-full", "bd-without-arms",
																   "bd-full"};

enum class BodyControl
{
	Hips,
	Chest,
	Head,
	LeftFoot,
	RightFoot,
	Count
};

enum class BodyPreset
{
	Standing,
	TPose,
	Seated
};

struct BodyControlOverride
{
	std::optional<XrVector3f> offset;
	bool tracked = true;
};

struct BodyJointOverride
{
	std::optional<XrPosef> pose;
	bool tracked = true;
};

struct BodyControls
{
	bool active = true;
	float confidence = 1;
	BodyPreset preset = BodyPreset::Standing;
	std::optional<XrPosef> rootPose;
	std::array<BodyControlOverride, static_cast<std::size_t>(BodyControl::Count)> controls;
	std::array<std::optional<BodyJointOverride>, BodyJointCount> joints;
};

struct BodyProportions
{
	float height = 1.7f;
	float shoulderWidth = 0.38f;
	float hipWidth = 0.24f;
	bool operator==(const BodyProportions&) const = default;
};

struct SkeletonJoint
{
	BodyJoint joint;
	std::optional<BodyJoint> parent;
	XrPosef bindPose;
};

struct ObservedBodyJoint
{
	XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
	XrSpaceLocationFlags flags = 0;
};

using ObservedBodySkeleton = std::array<std::optional<SkeletonJoint>, BodyJointCount>;

// Runtime-estimated body data in simulator world coordinates. Missing schema joints
// remain absent; this value does not contain generated rig fallback poses.
struct BodyObservation
{
	BodyJointSet jointSet = BodyJointSet::UpperBodyFB;
	RuntimeTime requestedTime;
	std::optional<RuntimeTime> returnedTime;
	bool located = false;
	std::optional<bool> active;
	std::optional<bool> allJointPosesTracked;
	std::optional<float> confidence;
	std::optional<std::uint32_t> skeletonRevision;
	std::array<std::optional<ObservedBodyJoint>, BodyJointCount> joints;
	std::optional<ObservedBodySkeleton> skeleton;
};

struct BodySample
{
	bool active = false;
	float confidence = 0;
	std::uint32_t skeletonRevision = 1;
	RuntimeTime sampleTime{};
	std::array<PoseSample, BodyJointCount> joints;
	std::array<SkeletonJoint, BodyJointCount> skeleton;
};

struct GazeInput
{
	float yaw = 0;
	float pitch = 0;
	bool available = true;
	bool tracked = true;
};

enum class TrackerRole
{
	Waist,
	Chest,
	LeftFoot,
	RightFoot,
	Unassigned,
	HandheldObject,
	LeftShoulder,
	RightShoulder,
	LeftElbow,
	RightElbow,
	LeftKnee,
	RightKnee,
	LeftWrist,
	RightWrist,
	LeftAnkle,
	RightAnkle,
	Camera,
	Keyboard
};

inline constexpr std::size_t MaxTrackerDevices = 64;

struct TrackerIdentity
{
	std::uint64_t value = 0;
	bool operator==(const TrackerIdentity&) const = default;
};

struct TrackerDevice
{
	TrackerIdentity identity;
	TrackerRole role;
	bool connected = true;
	bool tracked = true;
	std::optional<BodyJoint> anchor;
	std::optional<XrPosef> poseOverride;
	std::optional<XrPosef> jointOffset;
	XrSpaceLocationFlags poseFlags = TrackedPoseFlags;
	TrackerSourcePath sourcePath;
};

struct TrackerSample
{
	TrackerDevice device;
	PoseSample pose;
};

struct StereoEyeViews
{
	std::array<PoseSample, 2> poses;
	std::array<XrFovf, 2> fovs{{{-0.9f, 0.9f, 0.9f, -0.9f}, {-0.9f, 0.9f, 0.9f, -0.9f}}};
	std::uint64_t spaceRevision = 0;
};

struct TrackingState
{
	PoseSample head{{{0, 0, 0, 1}, {0, 1.7f, 0}}, TrackedPoseFlags};
	std::array<ControllerPose, 2> controllers{
		{{{{{0.70710678f, 0, 0, 0.70710678f}, {-0.2f, 1.4f, -0.4f}}, TrackedPoseFlags}, true},
		 {{{{0.70710678f, 0, 0, 0.70710678f}, {0.2f, 1.4f, -0.4f}}, TrackedPoseFlags}, true}}};
	GazeInput gaze;
	BodyControls body;
	bool focused = true;
	std::vector<TrackerDevice> trackers{{{1}, TrackerRole::Waist, true, true, BodyJoint::Hips},
										{{2}, TrackerRole::Chest, true, true, BodyJoint::Chest},
										{{3}, TrackerRole::LeftFoot, true, true, BodyJoint::LeftFootAnkle},
										{{4}, TrackerRole::RightFoot, true, true, BodyJoint::RightFootAnkle}};
	std::optional<StereoEyeViews> eyeViews;
	std::optional<BodyObservation> bodyObservation;
};

struct TrackingSnapshot
{
	RuntimeTime time;
	PoseSample head;
	std::array<ControllerPose, 2> controllers;
	PoseSample gaze;
	BodySample body;
	// Four legacy devices by default; published inventories may contain up to MaxTrackerDevices.
	std::vector<TrackerSample> trackers = std::vector<TrackerSample>(4);
	TrackingState input;
	BodyProportions proportions;
};
} // namespace simxr
