#pragma once

#include "simulation/InputState.h"
#include "simulation/PoseSequence.h"
#include "simulation/TrackingTypes.h"
#include "simulation/ViewConfiguration.h"

#include <array>
#include <cstddef>

namespace simxr
{
struct LegacyHeadAngles
{
	float yaw = 0;
	float pitch = 0;
	float roll = 0;
};

class LegacyInput
{
  public:
	using Sequence = PoseSequence;
	LegacyInput();
	void StartSequence(Sequence sequence, FrameId origin);
	bool AdvanceSequence(FrameId submitted);
	TrackingState Tracking(bool focused = true) const;
	std::array<ControllerControls, 2> Controls() const;
	const XrPosef& HeadPose() const;
	LegacyHeadAngles HeadAngles() const;
	const ViewSettings& Views() const;
	void SetViews(ViewSettings settings);

  private:
	struct Controller
	{
		XrPosef offset;
		ControllerControls controls;
	};

	XrPosef m_head{{0, 0, 0, 1}, {0, 1.7f, 0}};
	std::array<float, 3> m_headAngles{};
	std::array<Controller, 2> m_controllers;
	ViewSettings m_views;
	PoseSequencePlayback m_sequence;
};
} // namespace simxr
