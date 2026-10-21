#include "simulation/CommandState.h"
#include "simulation/TrackingSystem.h"

#include <stdexcept>
#include <type_traits>
#include <utility>

namespace simxr
{
namespace
{
constexpr std::size_t Index(ControlChannel channel)
{
	return static_cast<std::size_t>(channel);
}
constexpr std::size_t Index(FailureScenario scenario)
{
	return static_cast<std::size_t>(scenario);
}
} // namespace

ControlChannel CommandState::ScenarioChannel(FailureScenario scenario)
{
	switch (scenario)
	{
	case FailureScenario::HeadUntracked:
	case FailureScenario::HeadUnavailable:
		return ControlChannel::Head;
	case FailureScenario::BodyUntracked:
	case FailureScenario::BodyUnavailable:
		return ControlChannel::Body;
	case FailureScenario::GazeUntracked:
	case FailureScenario::GazeUnavailable:
		return ControlChannel::Gaze;
	case FailureScenario::LeftDisconnected:
		return ControlChannel::LeftPose;
	case FailureScenario::RightDisconnected:
		return ControlChannel::RightPose;
	case FailureScenario::TrackersDisconnected:
		return ControlChannel::Trackers;
	default:
		return ControlChannel::Session;
	}
}
ControlChannel CommandState::ChannelFor(const SetHeadPose&)
{
	return ControlChannel::Head;
}

ControlChannel CommandState::ChannelFor(const SetControllerPose& command)
{
	return command.hand == 0 ? ControlChannel::LeftPose : ControlChannel::RightPose;
}

ControlChannel CommandState::ChannelFor(const SetControllerControls& command)
{
	return command.hand == 0 ? ControlChannel::LeftControls : ControlChannel::RightControls;
}

ControlChannel CommandState::ChannelFor(const SetBodyControls&)
{
	return ControlChannel::Body;
}

ControlChannel CommandState::ChannelFor(const SetGaze&)
{
	return ControlChannel::Gaze;
}

ControlChannel CommandState::ChannelFor(const SetTracker&)
{
	return ControlChannel::Trackers;
}

ControlChannel CommandState::ChannelFor(const SetViewSettings&)
{
	return ControlChannel::Views;
}

ControlChannel CommandState::ChannelFor(const SetBodyProportions&)
{
	return ControlChannel::Proportions;
}

ControlChannel CommandState::ChannelFor(const SetControllerProfile&)
{
	return ControlChannel::Profiles;
}

ControlChannel CommandState::ChannelFor(const SetScenario& command)
{
	return ScenarioChannel(command.scenario);
}
ControlChannel CommandState::ChannelFor(const SetReferenceSpaces&)
{
	return ControlChannel::Session;
}

void CommandState::ApplyScenarios(CommandConfiguration& config) const
{
	if (m_scenarios[Index(FailureScenario::HeadUntracked)])
		config.tracking.head.flags &= ValidPoseFlags;
	if (m_scenarios[Index(FailureScenario::HeadUnavailable)])
		config.tracking.head.flags = 0;
	if (m_scenarios[Index(FailureScenario::BodyUntracked)])
	{
		for (auto& control : config.tracking.body.controls)
			control.tracked = false;
		for (auto& joint : config.tracking.body.joints)
			if (joint)
				joint->tracked = false;
	}
	if (m_scenarios[Index(FailureScenario::BodyUnavailable)])
		config.tracking.body.active = false;
	if (m_scenarios[Index(FailureScenario::GazeUntracked)])
		config.tracking.gaze.tracked = false;
	if (m_scenarios[Index(FailureScenario::GazeUnavailable)])
		config.tracking.gaze.available = false;
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		if (m_scenarios[Index(hand == 0 ? FailureScenario::LeftDisconnected : FailureScenario::RightDisconnected)])
		{
			config.tracking.controllers[hand].connected = false;
			config.input.controllers[hand].connected = false;
		}
	}
	if (m_scenarios[Index(FailureScenario::TrackersDisconnected)])
		for (auto& device : config.tracking.trackers)
			device.connected = false;
	if (m_scenarios[Index(FailureScenario::Unfocused)] || m_scenarios[Index(FailureScenario::Hidden)] ||
		m_scenarios[Index(FailureScenario::SessionLoss)])
		config.tracking.focused = false;
}
CommandConfiguration CommandState::MergeOverrides(const CommandConfiguration& baseline,
												  const CommandConfiguration& values, const ChannelMask& overrides)
{
	auto result = baseline;
	if (overrides[Index(ControlChannel::Head)])
		result.tracking.head = values.tracking.head;
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		if (overrides[Index(hand == 0 ? ControlChannel::LeftPose : ControlChannel::RightPose)])
			result.tracking.controllers[hand] = values.tracking.controllers[hand];
		if (overrides[Index(hand == 0 ? ControlChannel::LeftControls : ControlChannel::RightControls)])
			result.input.controllers[hand] = values.input.controllers[hand];
	}
	if (overrides[Index(ControlChannel::Body)])
		result.tracking.body = values.tracking.body;
	if (overrides[Index(ControlChannel::Gaze)])
		result.tracking.gaze = values.tracking.gaze;
	if (overrides[Index(ControlChannel::Trackers)])
		result.tracking.trackers = values.tracking.trackers;
	if (overrides[Index(ControlChannel::Views)])
		result.views = values.views;
	if (overrides[Index(ControlChannel::Proportions)])
		result.proportions = values.proportions;
	if (overrides[Index(ControlChannel::Profiles)])
		result.input.preferredProfiles = values.input.preferredProfiles;
	result.referenceSpaces = values.referenceSpaces;
	// Device connection is observable consistently through pose and action input.
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		const bool connected = result.input.controllers[hand].connected && result.tracking.controllers[hand].connected;
		result.input.controllers[hand].connected = connected;
		result.tracking.controllers[hand].connected = connected;
	}
	return result;
}
void CommandState::Normalize(CommandConfiguration& value, RuntimeTime time)
{
	if (!NormalizeReferenceSpaces(value.referenceSpaces))
		throw std::invalid_argument("Invalid reference-space origin");
	for (const auto& tracker : value.tracking.trackers)
		if (static_cast<unsigned>(tracker.role) > static_cast<unsigned>(TrackerRole::Keyboard))
			throw std::invalid_argument("Invalid tracker role");
	const BodyModel model(value.proportions);
	const auto normalized = TrackingSystem::Evaluate(time, value.tracking, model);
	value.tracking = normalized.input;
	const ViewConfiguration validatedViews(ViewLayout::StereoWithInset, value.views);
	if (value.input.devices.size() > 16 || XR_FAILED(InputState::Validate(value.input)))
		throw std::invalid_argument("Invalid command input");
	for (const auto& device : value.input.devices)
	{
		if (device.userPaths.size() > 8 || device.preferredProfile.size() > 255)
			throw std::invalid_argument("Command device metadata exceeds bounds");
		for (const auto& path : device.userPaths)
			if (path.size() > 255)
				throw std::invalid_argument("Command device path exceeds bounds");
	}
	for (const auto& profile : value.input.preferredProfiles)
	{
		if (!profile.starts_with("/interaction_profiles/") || profile.size() > 255 || profile.back() == '/' ||
			profile.find('\0') != std::string::npos)
			throw std::invalid_argument("Invalid command profile path");
		for (const char c : profile)
			if (!(c == '/' || c == '_' || c == '-' || c == '.' || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9')))
				throw std::invalid_argument("Invalid command profile path");
	}
}
void CommandState::ClearChannel(ControlChannel channel) noexcept
{
	m_overrides[Index(channel)] = false;
	// Release held controls instead of feeding the last synthetic press back into
	// the next native baseline. A new owner can subsequently replace this overlay.
	if (channel == ControlChannel::LeftControls || channel == ControlChannel::RightControls)
	{
		const auto hand = channel == ControlChannel::LeftControls ? 0 : 1;
		m_values.input.controllers[hand] = {};
		m_overrides[Index(channel)] = true;
	}
	for (std::size_t index = 0; index < m_scenarios.size(); ++index)
	{
		if (ScenarioChannel(static_cast<FailureScenario>(index)) == channel)
			m_scenarios[index] = false;
	}
}

class CommandState::Mutation
{
  public:
	Mutation(CommandState& state, CommandSource source, RuntimeTime time)
		: m_state(state), m_source(source), m_time(time)
	{
	}

	CommandResult operator()(const TakeControl& command)
	{
		const auto index = CheckedChannel(command.channel);
		const auto owner = m_state.m_owners[index];
		if (owner && owner != m_source && !command.replace)
			return CommandResult::Conflict;
		if (owner && owner != m_source)
			m_state.ClearChannel(command.channel);
		m_state.m_owners[index] = m_source;
		return CommandResult::Applied;
	}

	CommandResult operator()(const ReleaseControl& command)
	{
		const auto index = CheckedChannel(command.channel);
		if (m_state.m_owners[index] != m_source)
			return CommandResult::Conflict;
		m_state.ClearChannel(command.channel);
		m_state.m_owners[index].reset();
		return CommandResult::Applied;
	}
	CommandResult operator()(const SetScenario& command)
	{
		if (m_state.m_owners[CheckedChannel(ChannelFor(command))] != m_source)
			return CommandResult::Conflict;
		m_state.Set(command);
		if (command.scenario == FailureScenario::Recenter && command.enabled)
		{
			auto effective = m_state.m_values;
			m_state.ApplyScenarios(effective);
			Normalize(effective, m_time);
			if (!RecenterReferenceSpaces(m_state.m_values.referenceSpaces, effective.tracking.head))
				return CommandResult::Invalid;
		}
		return CommandResult::Applied;
	}

	template <class Command> CommandResult operator()(const Command& command)
	{
		const auto index = CheckedChannel(ChannelFor(command));
		if (m_state.m_owners[index] != m_source)
			return CommandResult::Conflict;
		m_state.Set(command);
		if constexpr (!std::is_same_v<Command, SetScenario>)
			m_state.m_overrides[index] = true;
		return CommandResult::Applied;
	}

  private:
	CommandState& m_state;
	CommandSource m_source;
	const RuntimeTime m_time;
};

CommandState::CommandState(std::optional<CommandConfiguration> initial)
	: m_values(initial.value_or(CommandConfiguration{}))
{
	Normalize(m_values, {Duration{1}});
	m_overrides.fill(initial.has_value());
}

void CommandState::BeginBoundary(const ReferenceSpaceState& spaces)
{
	m_values.referenceSpaces = spaces;
}

std::size_t CommandState::CheckedChannel(ControlChannel channel)
{
	const auto index = Index(channel);
	if (index >= ControlChannelCount)
		throw std::invalid_argument("Invalid control channel");
	return index;
}

void CommandState::Set(const SetHeadPose& command)
{
	m_values.tracking.head = command.value;
}
void CommandState::Set(const SetControllerPose& command)
{
	if (command.hand >= m_values.tracking.controllers.size())
		throw std::invalid_argument("Invalid controller");
	m_values.tracking.controllers[command.hand] = command.value;
}
void CommandState::Set(const SetControllerControls& command)
{
	if (command.hand >= m_values.input.controllers.size())
		throw std::invalid_argument("Invalid controller");
	m_values.input.controllers[command.hand] = command.value;
}
void CommandState::Set(const SetBodyControls& command)
{
	m_values.tracking.body = command.value;
}
void CommandState::Set(const SetGaze& command)
{
	m_values.tracking.gaze = command.value;
}
void CommandState::Set(const SetTracker& command)
{
	if (command.index >= m_values.tracking.trackers.size())
		throw std::invalid_argument("Invalid tracker");
	m_values.tracking.trackers[command.index] = command.value;
}
void CommandState::Set(const SetViewSettings& command)
{
	m_values.views = command.value;
}
void CommandState::Set(const SetBodyProportions& command)
{
	m_values.proportions = command.value;
}
void CommandState::Set(const SetControllerProfile& command)
{
	if (command.hand >= m_values.input.preferredProfiles.size())
		throw std::invalid_argument("Invalid controller");
	m_values.input.preferredProfiles[command.hand] = command.path;
}
void CommandState::Set(const SetScenario& command)
{
	if (Index(command.scenario) >= m_scenarios.size())
		throw std::invalid_argument("Invalid scenario");
	m_scenarios[Index(command.scenario)] = command.enabled;
}
void CommandState::Set(const SetReferenceSpaces& command)
{
	m_values.referenceSpaces = command.value;
}

void CommandState::NormalizeAcknowledged(SimulationCommand& command, const CommandConfiguration& configuration)
{
	if (auto* head = std::get_if<SetHeadPose>(&command))
		head->value = configuration.tracking.head;
	if (auto* pose = std::get_if<SetControllerPose>(&command))
		pose->value = configuration.tracking.controllers[pose->hand];
	if (auto* gaze = std::get_if<SetGaze>(&command))
		gaze->value = configuration.tracking.gaze;
	if (auto* body = std::get_if<SetBodyControls>(&command))
		body->value = configuration.tracking.body;
	if (auto* tracker = std::get_if<SetTracker>(&command))
		tracker->value = configuration.tracking.trackers[tracker->index];
	if (auto* spaces = std::get_if<SetReferenceSpaces>(&command))
		spaces->value = configuration.referenceSpaces;
}

CommandResult CommandState::Apply(CommandTransaction& transaction, const CommandConfiguration& baseline,
								  RuntimeTime time)
{
	CommandState candidate = *this;
	candidate.m_values = MergeOverrides(baseline, m_values, m_overrides);
	auto acknowledged = transaction.commands;
	try
	{
		Mutation mutate(candidate, transaction.source, time);
		for (auto& command : acknowledged)
		{
			const auto result = std::visit(mutate, command);
			if (result != CommandResult::Applied)
				return result;
			auto normalized = MergeOverrides(baseline, candidate.m_values, candidate.m_overrides);
			Normalize(normalized, time);
			candidate.m_values = std::move(normalized);
			NormalizeAcknowledged(command, candidate.m_values);
		}
		auto scenarioState = candidate.m_values;
		candidate.ApplyScenarios(scenarioState);
		Normalize(scenarioState, time);
	}
	catch (const std::invalid_argument&)
	{
		return CommandResult::Invalid;
	}
	Swap(candidate);
	transaction.commands.swap(acknowledged);
	return CommandResult::Applied;
}

CommandConfiguration CommandState::Merge(const CommandConfiguration& baseline) const
{
	auto configuration = MergeOverrides(baseline, m_values, m_overrides);
	ApplyScenarios(configuration);
	return configuration;
}

CommandRecordingOrigin CommandState::RecordingOrigin(const CommandConfiguration& baseline, RuntimeTime time) const
{
	CommandRecordingOrigin origin;
	origin.configuration = MergeOverrides(baseline, m_values, m_overrides);
	Normalize(origin.configuration, time);
	origin.scenarios = m_scenarios;
	return origin;
}

void CommandState::Capture(AppliedCommandSnapshot& snapshot, const CommandConfiguration& baseline,
						   RuntimeTime time) const
{
	auto configuration = Merge(baseline);
	Normalize(configuration, time);
	snapshot.configuration = std::move(configuration);
	snapshot.owners = m_owners;
	snapshot.scenarios = m_scenarios;
}

CommandState::ChannelMask CommandState::OwnedChannels(CommandSource source) const noexcept
{
	ChannelMask owned{};
	for (std::size_t index = 0; index < m_owners.size(); ++index)
		owned[index] = m_owners[index] == source;
	return owned;
}

CommandState::ChannelMask CommandState::ReleaseSource(CommandSource source) noexcept
{
	ChannelMask released{};
	for (std::size_t index = 0; index < m_owners.size(); ++index)
	{
		if (m_owners[index] != source)
			continue;
		ClearChannel(static_cast<ControlChannel>(index));
		m_owners[index].reset();
		released[index] = true;
	}
	return released;
}

void CommandState::ClearOwnership() noexcept
{
	m_owners.fill({});
	m_overrides.fill(false);
	m_scenarios.fill(false);
}

void CommandState::Swap(CommandState& other) noexcept
{
	static_assert(std::is_nothrow_swappable_v<CommandConfiguration>);
	using std::swap;
	swap(m_values, other.m_values);
	m_overrides.swap(other.m_overrides);
	m_owners.swap(other.m_owners);
	m_scenarios.swap(other.m_scenarios);
}
} // namespace simxr
