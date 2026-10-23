#pragma once

#include "app/SimulationEdits.h"
#include <array>
#include <optional>
#include <span>

namespace simxr::app
{
enum class Anchor
{
	Body,
	Head,
	World
};
enum class HandPreset
{
	Rest,
	Forward,
	Up
};
// Whole-body poses: the skeleton, both hands and attached trackers move together.
enum class BodyPose
{
	Stand,
	TPose,
	APose,
	Sit
};
inline constexpr std::array<const char*, 4> BodyPoseNames{"Stand", "T-pose", "A-pose", "Sit"};
struct AnchoredPose
{
	Anchor anchor = Anchor::Body;
	XrPosef offset{{0, 0, 0, 1}, {0, 0, 0}};
};
// A tracker follows a skeleton joint with an offset in joint space, or stays put in the world.
struct TrackerAttachment
{
	std::optional<BodyJoint> joint;
	XrPosef offset{{0, 0, 0, 1}, {0, 0, 0}};
};
const char* TrackerRoleName(TrackerRole role);
// One letter for markers: W, C, L, R and so on.
char TrackerRoleLetter(TrackerRole role);
const char* BodyJointName(BodyJoint joint);
// Joints a tracker can be attached to (no fingers).
std::span<const BodyJoint> AttachableJoints();
struct PosedBody
{
	BodyControls body;
	// Hand poses relative to the root, on the posed palms.
	std::array<XrPosef, 2> hands;
};
// Poses the skeleton in root space. Arms are set through joint overrides so the hands stay on the wrists.
PosedBody PoseBody(BodyPose pose, const BodyControls& current, const BodyProportions& proportions, float headHeight);

class PlayerRig
{
  public:
	void Reset(const ObservedSetup& observed);
	void Observe(const ObservedSetup& observed);
	bool Ready() const
	{
		return m_ready;
	}
	XrPosef AnchorPose(Anchor anchor) const;
	XrPosef Root() const;
	XrPosef Head() const;
	XrPosef Hand(std::size_t hand) const;
	XrPosef Tracker(std::size_t slot) const;
	// A skeleton joint as the runtime evaluates it for the rig's current head, root and body.
	XrPosef JointPose(BodyJoint joint) const;
	void SetHeadWorld(XrPosef pose);
	void SetHandWorld(std::size_t hand, XrPosef pose);
	void SetTrackerWorld(std::size_t slot, XrPosef pose);
	void SetHandAnchor(std::size_t hand, Anchor anchor);
	// Keeps the tracker where it is; no joint attaches it to the world.
	void AttachTracker(std::size_t slot, std::optional<BodyJoint> joint);
	// Back on its role's joint with no offset.
	void SnapTracker(std::size_t slot);
	void SetTrackerOffset(std::size_t slot, XrPosef offset);
	Anchor HandAnchor(std::size_t hand) const
	{
		return m_hands.at(hand).anchor;
	}
	const TrackerAttachment& TrackerAt(std::size_t slot) const
	{
		return m_trackers.at(slot);
	}
	const TrackerDevice& TrackerDeviceAt(std::size_t slot) const
	{
		return m_trackerDevices.at(slot);
	}
	void Preset(std::size_t hand, HandPreset preset);
	void ApplyBodyPose(BodyPose pose);
	const BodyControls& Body() const
	{
		return m_body;
	}
	std::optional<BodyPose> CurrentBodyPose() const
	{
		return m_bodyPose;
	}
	void ResetHead();
	void ResetHand(std::size_t hand);
	void ResetBody();
	void ResetTracker(std::size_t slot);
	// Root, head, hands, body and trackers back to defaults; settings stay.
	void ResetAll();
	void Move(XrVector3f localVelocity, float yaw, float pitch, float seconds, bool freeLook = false);
	ManualTransaction Transaction(const ObservedSetup& observed, std::optional<CommandSource> ownSource,
								  bool includeWorld = false) const;
	XrVector3f position{};
	float facing = 0, walkSpeed = 1, height = 1.7f;

  private:
	void ObserveTrackers(const ObservedSetup& observed);
	bool m_ready = false;
	XrVector3f m_lean{};
	float m_headYaw = 0, m_headPitch = 0;
	XrQuaternionf m_headRoll{0, 0, 0, 1};
	std::array<AnchoredPose, 2> m_hands;
	std::array<TrackerAttachment, 4> m_trackers;
	std::array<TrackerDevice, 4> m_trackerDevices = TrackingState{}.trackers;
	BodyControls m_body;
	BodyProportions m_proportions;
	bool m_bodyRoot = true;
	std::optional<BodyPose> m_bodyPose = BodyPose::Stand;
};
} // namespace simxr::app
