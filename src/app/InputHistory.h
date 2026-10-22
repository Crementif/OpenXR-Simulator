#pragma once

#include "app/ShellState.h"
#include "automation/Recording.h"
#include "simulation/CommandTypes.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace simxr::app
{
// The applied head/hand input of one frame, as the runtime reported it.
struct InputSample
{
	FrameIdentity identity;
	std::uint64_t epoch = 0, seed = 0;
	PoseSample head;
	std::array<ControllerPose, 2> poses;
	std::array<ControllerControls, 2> controls;
	ScenarioState scenarios{};
	// Channels owned by a script, MCP or replay, by ControlChannel bit.
	std::uint32_t driven = 0;
	std::uint32_t actions = 0;
};
// How strongly a hand is pressed, 0..1, for the timeline sparkline.
float Intensity(const ControllerControls& controls);

// Fixed ring of input samples addressed by a rising sequence number, so a
// history frame keeps its sample after the ring has moved on (or reports it gone).
class InputHistory
{
  public:
	explicit InputHistory(std::size_t capacity = 32768);
	std::uint64_t Append(InputSample sample);
	const InputSample* Find(std::uint64_t sequence) const noexcept;
	void Clear() noexcept;

  private:
	std::vector<InputSample> m_samples;
	std::uint64_t m_next = 1;
};

// A take with the same structure the runtime recorder exports: the first sample
// and the setup are its origin, later samples become transactions where they change.
automation::InputRecording TakeFromInput(const std::vector<const InputSample*>& samples,
										 const CommandConfiguration& setup);
} // namespace simxr::app
