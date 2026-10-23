#pragma once

#include "simulation/CommandTypes.h"
#include "simulation/PoseSequence.h"

#include <array>
#include <cstdint>
#include <memory>
#include <vector>

namespace simxr
{
struct PoseSequenceStatus
{
	std::uint64_t id = 0, groups = 0;
	CommandSource owner;
	CommandResult result = CommandResult::Queued;
	FrameId origin, lastFrame;
	bool active = true, loop = false;
};
struct PoseSequenceFrame
{
	std::vector<SimulationCommand> commands;
	bool capture = false, advanced = false;
};

// Copied with a prepared command boundary. The large immutable program is shared;
// cursor and pose state publish only after the frame's reservation succeeds.
class PoseSequenceExecution
{
  public:
	PoseSequenceExecution(std::uint64_t id, CommandSource owner, std::shared_ptr<const PoseSequence> program,
						  std::array<bool, ControlChannelCount> inherited = {});
	PoseSequenceFrame Prepare(FrameId frame, const CommandConfiguration& initial);
	void Accept(FrameId frame, CommandResult result) noexcept;
	void Cancel() noexcept;
	bool CanPrepare() const noexcept;
	bool HasControl(const std::array<bool, ControlChannelCount>& owned) const noexcept;
	const PoseSequenceStatus& Status() const noexcept;
	CommandSource Source() const noexcept;

  private:
	void Initialize(FrameId frame, const CommandConfiguration& initial);
	void Patch(const PoseSequenceStep& step);
	std::vector<SimulationCommand> Commands() const;
	std::shared_ptr<const PoseSequence> m_program;
	PoseSequencePlayback m_playback;
	PoseSequenceStatus m_status;
	std::array<bool, 5> m_channels{};
	PoseSample m_head;
	std::array<ControllerPose, 2> m_controllers;
	std::array<ControllerControls, 2> m_controls;
	std::array<XrPosef, 2> m_local;
	std::array<float, 3> m_angles{};
	bool m_started = false;
};
} // namespace simxr
