#pragma once

#include "presentation/FrameTypes.h"
#include "simulation/InputState.h"
#include "simulation/TrackingTypes.h"
#include "simulation/ViewConfiguration.h"

#include <array>
#include <cstddef>
#include <iosfwd>
#include <optional>
#include <string>
#include <vector>

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
	class Sequence;
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
	struct HeadUpdate
	{
		std::array<std::optional<float>, 3> position;
		std::array<std::optional<float>, 3> angles;
	};
	struct ControllerUpdate
	{
		std::array<std::optional<float>, 3> position;
		std::optional<XrQuaternionf> orientation;
		std::optional<float> trigger;
		std::optional<float> grip;
		std::array<std::optional<float>, 2> stick;
		std::optional<bool> primary;
		std::optional<bool> secondary;
	};
	struct SequenceStep
	{
		FrameId frame;
		std::optional<HeadUpdate> head;
		std::array<std::optional<ControllerUpdate>, 2> controllers;
		bool capture = false;
	};

  public:
	class Sequence
	{
	  public:
		std::size_t Size() const
		{
			return m_steps.size();
		}

	  private:
		friend class LegacyInput;
		std::vector<SequenceStep> m_steps;
		bool m_loop = false;
	};

  private:
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
	std::vector<SequenceStep> m_sequence;
	FrameId m_sequenceOrigin;
	std::size_t m_sequenceIndex = 0;
	bool m_sequenceLoop = false;
};
} // namespace simxr
