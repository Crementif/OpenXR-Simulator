#pragma once

#include "simulation/InputState.h"
#include "simulation/PoseSequence.h"
#include "simulation/TrackingTypes.h"
#include "simulation/ViewConfiguration.h"

#include <array>
#include <cstddef>
#include <iosfwd>
#include <string>

namespace json
{
class Object;
}

namespace simxr
{
enum class LegacyInputCommand
{
	HeadPose,
	ControllerPose,
	ControllerHold,
	Ipd,
	Fov
};

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
	void Apply(LegacyInputCommand command, const std::string& document);
	static Sequence ReadSequence(std::istream& input, bool loop);
	void StartSequence(Sequence sequence, FrameId origin);
	bool AdvanceSequence(FrameId submitted);
	TrackingState Tracking(bool focused = true) const;
	std::array<ControllerControls, 2> Controls() const;
	const XrPosef& HeadPose() const;
	LegacyHeadAngles HeadAngles() const;
	const ViewSettings& Views() const;
	void SetViews(ViewSettings settings);

  private:
	using HeadUpdate = HeadPosePatch;
	using ControllerUpdate = ControllerPosePatch;
	struct Controller
	{
		XrPosef offset;
		ControllerControls controls;
	};
	static HeadUpdate DecodeHead(const json::Object& document);
	static ControllerUpdate DecodeController(const json::Object& document);
	void ApplyHead(const HeadUpdate& update);
	void ApplyController(std::size_t hand, const ControllerUpdate& update);
	void ApplyControllerHold(const json::Object& document);

	XrPosef m_head{{0, 0, 0, 1}, {0, 1.7f, 0}};
	LegacyHeadAngles m_headAngles;
	std::array<Controller, 2> m_controllers;
	ViewSettings m_views;
	PoseSequencePlayback m_sequence;
};
} // namespace simxr
