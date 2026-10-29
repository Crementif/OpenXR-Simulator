#pragma once

#include "app/services/FrameIdentity.h"
#include "automation/Recording.h"
#include "simulation/CommandTypes.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <map>
#include <memory>
#include <optional>
#include <vector>

namespace simxr::app
{
using TrackerDevices = std::vector<TrackerDevice>;

// The applied device input of one frame, as the runtime reported it.
struct InputSample
{
	FrameIdentity identity;
	std::uint64_t epoch = 0, seed = 0;
	// Only complete applied-device records can become a replayable take.
	bool complete = false;
	PoseSample head;
	std::array<ControllerPose, 2> poses;
	std::array<ControllerControls, 2> controls;
	// Empty when the runtime did not report them. The history shares an
	// unchanged block with the previous sample, so equal pointers mean equal values.
	// The body root moves with the player every frame, so it lives in bodyRoot
	// and the shared block never has one.
	std::shared_ptr<const BodyControls> body;
	std::optional<XrPosef> bodyRoot;
	// Exact provider sample, including its time and validity; absent for authored bodies.
	std::shared_ptr<const BodyObservation> bodyObservation;
	std::shared_ptr<const TrackerDevices> trackers;
	std::optional<GazeInput> gaze;
	// Encoded frame-local view/profile/proportion/reference-space commands.
	// Equal contexts share storage; absent on older runtimes.
	std::shared_ptr<const std::vector<std::byte>> context;
	ScenarioState scenarios{};
	// Channels owned by a script, MCP or replay, by ControlChannel bit.
	std::uint32_t driven = 0;
	std::uint32_t actions = 0;
	std::optional<RuntimeTime> runtimeTime;
};

// The sample's body controls with their root; requires sample.body.
BodyControls BodyOf(const InputSample& sample);
// How strongly a hand is pressed, 0..1, for the timeline sparkline.
float Intensity(const ControllerControls& controls);

// Slot- and byte-bounded input samples addressed by a rising sequence number, so a
// history frame keeps its sample after the ring has moved on (or reports it gone).
class InputHistory
{
public:
	explicit InputHistory(std::size_t capacity = 32768, std::size_t byteBudget = 64 * 1024 * 1024);

	const InputSample* Find(std::uint64_t sequence) const noexcept;
	// Owned record storage, counting shared blocks once and including allocation allowances.
	std::size_t RetainedBytes() const noexcept;
	std::size_t ByteBudget() const noexcept;

	// Body and tracker values equal to the previous sample's reuse its block.
	std::uint64_t Append(InputSample sample);
	void Clear() noexcept;

private:
	struct Block
	{
		std::size_t bytes = 0, references = 0;
	};

	void Retain(const void* identity, std::size_t bytes);
	void Release(const void* identity) noexcept;
	void ReleaseSample(const InputSample& sample) noexcept;
	void Evict() noexcept;

	std::vector<InputSample> m_samples;
	std::uint64_t m_next = 1;
	std::size_t m_size = 0;
	std::map<const void*, Block> m_blocks;
	std::size_t m_bytes = 0, m_byteBudget = 0;
};

// A take with the same structure the runtime recorder exports: the first sample
// and its recorded views, proportions, profiles and spaces are its origin; later
// samples become transactions where they change. Every sample must carry all
// device channels and complete configuration context.
automation::InputRecording TakeFromInput(const std::vector<const InputSample*>& samples);
} // namespace simxr::app
