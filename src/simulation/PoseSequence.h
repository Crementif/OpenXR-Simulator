#pragma once

#include "presentation/FrameTypes.h"

#include <array>
#include <cstddef>
#include <memory>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include <openxr/openxr.h>

namespace simxr
{
struct ControllerControls;

struct HeadPosePatch
{
	std::array<std::optional<float>, 3> position;
	std::array<std::optional<float>, 3> angles;
	void Apply(XrPosef& pose, std::array<float, 3>& currentAngles) const;
};

struct ControllerPosePatch
{
	std::array<std::optional<float>, 3> position;
	std::optional<XrQuaternionf> orientation;
	std::optional<float> trigger;
	std::optional<float> grip;
	std::array<std::optional<float>, 2> stick;
	std::optional<bool> primary;
	std::optional<bool> secondary;
	void Apply(XrPosef& pose, ControllerControls& controls) const;
};

struct PoseSequenceStep
{
	FrameId frame;
	// Elapsed simulation time of a time-keyed program's row.
	Duration time{};
	std::optional<HeadPosePatch> head;
	std::array<std::optional<ControllerPosePatch>, 2> controllers;
	bool capture = false;
};

// Head x, y, z, yaw, pitch, roll, then per hand x, y, z, trigger, grip, stick x, stick y.
inline constexpr std::size_t PoseSequenceScalarTracks = 20;

// Continuous values of a time-keyed program; head-relative for controllers.
struct PoseSequenceTrackValues
{
	std::array<float, PoseSequenceScalarTracks> scalars{};
	std::array<XrQuaternionf, 2> orientations{};
};

struct PoseSequenceSample
{
	HeadPosePatch head;
	std::array<ControllerPosePatch, 2> controllers;
};

class PoseSequence
{
public:
	static constexpr std::size_t MaxSteps = 100000;
	static constexpr Duration MaxTime = std::chrono::hours(24 * 365);
	static PoseSequence Parse(std::string_view source, bool loop);
	PoseSequence() = default;
	PoseSequence(std::vector<PoseSequenceStep> steps, bool loop, bool timed = false);
	std::size_t Size() const noexcept;
	std::span<const PoseSequenceStep> Steps() const noexcept;
	bool Loops() const noexcept;
	const std::array<bool, 5>& Channels() const noexcept;

	// Time-keyed rows interpolate every keyed continuous value linearly (controller
	// orientation spherically) between that value's own keyframes. Before its first
	// keyframe a value moves from `start`, the value when the cycle began.
	bool Timed() const noexcept;
	Duration Length() const noexcept;
	PoseSequenceSample Sample(Duration elapsed, const PoseSequenceTrackValues& start) const;
	PoseSequenceTrackValues End(const PoseSequenceTrackValues& start) const;

private:
	struct ScalarKey
	{
		Duration time;
		float value;
	};

	struct OrientationKey
	{
		Duration time;
		XrQuaternionf value;
	};

	std::vector<PoseSequenceStep> m_steps;
	bool m_loop = false, m_timed = false;
	std::array<bool, 5> m_channels{};
	std::array<std::vector<ScalarKey>, PoseSequenceScalarTracks> m_scalars;
	std::array<std::vector<OrientationKey>, 2> m_orientations;
};

// Owns the decoded program. Returned steps remain valid until the next Start.
class PoseSequencePlayback
{
public:
	void Start(PoseSequence sequence, FrameId origin);
	void Start(std::shared_ptr<const PoseSequence> sequence, FrameId origin);
	std::span<const PoseSequenceStep> Advance(FrameId submitted) noexcept;
	std::optional<FrameId> NextFrame() const noexcept;
	bool Complete() const noexcept;

private:
	std::shared_ptr<const PoseSequence> m_sequence;
	FrameId m_origin;
	std::size_t m_next = 0;
};
} // namespace simxr
