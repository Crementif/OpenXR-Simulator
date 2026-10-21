#pragma once

#include "app/SimulationEdits.h"
#include <array>

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
	Up,
	TPose
};
struct AnchoredPose
{
	Anchor anchor = Anchor::Body;
	XrPosef offset{{0, 0, 0, 1}, {0, 0, 0}};
};
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
	void SetHeadWorld(XrPosef pose);
	void SetHandWorld(std::size_t hand, XrPosef pose);
	void SetTrackerWorld(std::size_t slot, XrPosef pose);
	void SetHandAnchor(std::size_t hand, Anchor anchor);
	void SetTrackerAnchor(std::size_t slot, Anchor anchor);
	Anchor HandAnchor(std::size_t hand) const
	{
		return m_hands.at(hand).anchor;
	}
	Anchor TrackerAnchor(std::size_t slot) const
	{
		return m_trackers.at(slot).anchor;
	}
	void Preset(std::size_t hand, HandPreset preset);
	void Move(XrVector3f localVelocity, float yaw, float pitch, float seconds, bool freeLook = false);
	ManualTransaction Transaction(const ObservedSetup& observed, std::optional<CommandSource> ownSource,
								  bool includeWorld = false) const;
	XrVector3f position{};
	float facing = 0, walkSpeed = 1, height = 1.7f;

  private:
	bool m_ready = false;
	XrVector3f m_lean{};
	float m_headYaw = 0, m_headPitch = 0;
	XrQuaternionf m_headRoll{0, 0, 0, 1};
	std::array<AnchoredPose, 2> m_hands;
	std::array<AnchoredPose, 4> m_trackers;
};
} // namespace simxr::app
