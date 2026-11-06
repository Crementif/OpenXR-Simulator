#pragma once

#include "simulation/CommandTypes.h"

#include <array>
#include <cstddef>
#include <optional>

namespace simxr
{
// Owns the effective command overlay. Queueing, request identity and recording
// belong to CommandScheduler; this class enforces channel ownership and atomic
// application of one transaction. Its caller supplies synchronization.
class CommandState
{
public:
	using ChannelMask = std::array<bool, ControlChannelCount>;

	CommandState() = default;
	explicit CommandState(std::optional<CommandConfiguration> initial);

	static ControlChannel ScenarioChannel(FailureScenario scenario);

	// Advances velocity-driven head motion to the boundary's display time.
	void BeginBoundary(const ReferenceSpaceState& spaces, RuntimeTime time);
	CommandResult Apply(CommandTransaction& transaction, const CommandConfiguration& baseline, RuntimeTime time);
	CommandConfiguration Merge(const CommandConfiguration& baseline) const;
	CommandRecordingOrigin RecordingOrigin(const CommandConfiguration& baseline, RuntimeTime time) const;
	void Capture(AppliedCommandSnapshot& snapshot, const CommandConfiguration& baseline, RuntimeTime time) const;

	ChannelMask OwnedChannels(CommandSource source) const noexcept;
	ChannelMask ReleaseSource(CommandSource source) noexcept;
	void ClearOwnership() noexcept;
	void Swap(CommandState& other) noexcept;

private:
	class Mutation;
	using ChannelOwners = std::array<std::optional<CommandSource>, ControlChannelCount>;
	using ScenarioMask = ScenarioState;

	static ControlChannel ChannelFor(const SetHeadPose& command);
	static ControlChannel ChannelFor(const SetControllerPose& command);
	static ControlChannel ChannelFor(const SetControllerControls& command);
	static ControlChannel ChannelFor(const SetBodyControls& command);
	static ControlChannel ChannelFor(const SetGaze& command);
	static ControlChannel ChannelFor(const SetTracker& command);
	static ControlChannel ChannelFor(const SetTrackers& command);
	static ControlChannel ChannelFor(const SetViewSettings& command);
	static ControlChannel ChannelFor(const SetEyeViews& command);
	static ControlChannel ChannelFor(const SetBodyProportions& command);
	static ControlChannel ChannelFor(const SetControllerProfile& command);
	static ControlChannel ChannelFor(const SetScenario& command);
	static ControlChannel ChannelFor(const SetReferenceSpaces& command);

	static std::size_t CheckedChannel(ControlChannel channel);
	static void Normalize(CommandConfiguration& configuration, RuntimeTime time);
	static void NormalizeAcknowledged(SimulationCommand& command, const CommandConfiguration& configuration);
	static CommandConfiguration MergeOverrides(const CommandConfiguration& baseline, const CommandConfiguration& values,
											   const ChannelMask& overrides);
	void ApplyScenarios(CommandConfiguration& configuration) const;
	void ClearChannel(ControlChannel channel) noexcept;

	void Set(const SetHeadPose& command);
	void Set(const SetControllerPose& command);
	void Set(const SetControllerControls& command);
	void Set(const SetBodyControls& command);
	void Set(const SetGaze& command);
	void Set(const SetTracker& command);
	void Set(const SetTrackers& command);
	void Set(const SetViewSettings& command);
	void Set(const SetEyeViews& command);
	void Set(const SetBodyProportions& command);
	void Set(const SetControllerProfile& command);
	void Set(const SetScenario& command);
	void Set(const SetReferenceSpaces& command);

	struct HeadMotion
	{
		XrPosef origin;
		RuntimeTime time;
		XrVector3f linear, angular;
	};

	CommandConfiguration m_values;
	std::optional<HeadMotion> m_headMotion;
	ChannelMask m_overrides{};
	ChannelOwners m_owners{};
	ScenarioMask m_scenarios{};
};
} // namespace simxr
