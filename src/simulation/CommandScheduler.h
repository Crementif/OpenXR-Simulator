#pragma once

#include "simulation/CommandRecorder.h"
#include "simulation/CommandState.h"
#include "simulation/LiveInputSlot.h"
#include "simulation/PoseSequenceExecution.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace simxr
{
class CommandScheduler
{
public:
	explicit CommandScheduler(std::size_t pendingCapacity = 128, std::size_t historyCapacity = 256);

	void Start(std::uint64_t seed = 0, std::optional<CommandConfiguration> initial = {});
	void Close();

	CommandOutcome Submit(CommandTransaction transaction);
	CommandResult SubmitLiveInput(CommandSource source, const LiveInputSample& sample,
								  LiveInputClock::time_point now = LiveInputClock::now());
	LiveInputStatus LiveInputState() const;
	void AcknowledgeLiveInput();
	std::array<bool, ControlChannelCount> ExpireLiveInput(LiveInputClock::time_point now);

	std::optional<PoseSequenceStatus> StartPoseSequence(CommandSource owner,
														std::shared_ptr<const PoseSequence> program);
	std::optional<PoseSequenceStatus> PoseSequenceStatusFor(CommandSource owner) const;
	std::array<bool, ControlChannelCount> StopPoseSequence(CommandSource owner);

	std::optional<CommandOutcome> Read(CommandSource source, std::uint64_t id) const;
	CommandSchedulerSnapshot Snapshot() const;
	CommandResult Cancel(CommandSource source, std::uint64_t id);
	std::array<bool, ControlChannelCount> ReleaseSource(CommandSource source, bool retire = false);

	// One commit per increasing frame; later native publications use Merge only.
	// reserve runs while command admission is locked. It may inspect the recorder,
	// but must not reenter this scheduler or mutate recording. It must complete all
	// throwing external allocation before its own publication/acceptance.
	std::shared_ptr<const AppliedCommandSnapshot>
	Apply(FrameId frame, SimulationTime time, RuntimeTime runtimeTime, const CommandConfiguration& baseline,
		  const std::function<void(const std::shared_ptr<const AppliedCommandSnapshot>&)>& reserve = {},
		  LiveInputClock::time_point now = LiveInputClock::now(), std::optional<std::uint64_t> clockRevision = {});
	CommandConfiguration Merge(const CommandConfiguration& baseline) const;
	std::shared_ptr<const AppliedCommandSnapshot> At(FrameId frame) const;

	// Capture origin and start recording under command admission serialization.
	RecordingResult StartRecording(const CommandConfiguration& baseline, const ReferenceSpaceState& spaces,
								   bool drain = false, CommandSource owner = ApplicationAutomationSource);
	RecordingResult StopRecording(CommandSource owner, std::uint64_t identity = 0);
	RecordingResult AcknowledgeRecording(CommandSource owner, std::uint64_t identity, std::size_t through);
	CommandRecorder& Recorder();

private:
	class PreparedCommandBoundary;

	struct SourceCursor
	{
		CommandSource source;
		std::uint64_t last = 0;
	};

	std::array<bool, ControlChannelCount> ReleaseSourceCommandsLocked(CommandSource source, bool retire);
	std::array<bool, ControlChannelCount> ReleaseSourceLocked(CommandSource source) noexcept;
	std::array<bool, ControlChannelCount> StopPoseSequenceLocked(CommandSource owner) noexcept;

	const std::size_t m_pendingCapacity, m_historyCapacity;
	mutable std::mutex m_mutex;
	bool m_closed = false;
	std::uint64_t m_seed = 0, m_revision = 0;
	CommandState m_state;
	std::optional<CommandConfiguration> m_baseline;
	std::deque<CommandOutcome> m_pending, m_outcomes;
	std::vector<SourceCursor> m_sources;
	std::deque<std::shared_ptr<const AppliedCommandSnapshot>> m_applied;
	CommandRecorder m_recorder;
	std::optional<CommandSource> m_recordingOwner;
	LiveInputSlot m_liveInput;
	std::optional<PoseSequenceExecution> m_sequence;
	std::optional<CommandSource> m_replacedSequence;
	std::uint64_t m_nextSequence = 1;
};
} // namespace simxr
