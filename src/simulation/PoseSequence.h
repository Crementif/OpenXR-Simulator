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
	std::optional<HeadPosePatch> head;
	std::array<std::optional<ControllerPosePatch>, 2> controllers;
	bool capture = false;
};

class PoseSequence
{
public:
	static constexpr std::size_t MaxSteps = 100000;
	static PoseSequence Parse(std::string_view source, bool loop);
	PoseSequence() = default;
	PoseSequence(std::vector<PoseSequenceStep> steps, bool loop);
	std::size_t Size() const noexcept;
	std::span<const PoseSequenceStep> Steps() const noexcept;
	bool Loops() const noexcept;
	const std::array<bool, 5>& Channels() const noexcept;

private:
	std::vector<PoseSequenceStep> m_steps;
	bool m_loop = false;
	std::array<bool, 5> m_channels{};
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
