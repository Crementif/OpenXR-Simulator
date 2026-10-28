#include "app/services/UndoStack.h"
#include "Pch.h"

namespace simxr::app
{
void UndoStack::Observe(const ObservedSetup& observed)
{
	if (!m_open)
		m_authored = State{observed.values.tracking, observed.values.proportions};
}
void UndoStack::Record(const ObservedSetup& before, const ManualTransaction& edit)
{
	const bool pose = std::any_of(edit.commands.begin(), edit.commands.end(),
								  [](const SimulationCommand& command)
								  {
									  return std::holds_alternative<SetHeadPose>(command) ||
											 std::holds_alternative<SetControllerPose>(command) ||
											 std::holds_alternative<SetBodyControls>(command) ||
											 std::holds_alternative<SetGaze>(command) ||
											 std::holds_alternative<SetTracker>(command) ||
											 std::holds_alternative<SetTrackers>(command) ||
											 std::holds_alternative<SetBodyProportions>(command);
								  });
	if (!pose)
		return;
	if (!m_authored)
		m_authored = State{before.values.tracking, before.values.proportions};
	// The first edit of a drag opens the group; its continuous updates and final edit join it.
	if (!m_open)
	{
		constexpr std::size_t capacity = 64;
		if (m_entries.size() == capacity)
			m_entries.erase(m_entries.begin());
		m_entries.push_back(Entry{*m_authored});
		m_entries.back().time = std::chrono::steady_clock::now();
		m_entries.back().label = edit.label;
	}
	else if (m_entries.back().label.empty())
		m_entries.back().label = edit.label;
	auto& entry = m_entries.back();
	const auto remember = [&](ControlChannel channel)
	{
		entry.channels[static_cast<std::size_t>(channel)] = true;
	};
	for (const auto& command : edit.commands)
		std::visit(
			[&](const auto& value)
			{
				using T = std::decay_t<decltype(value)>;
				if constexpr (std::is_same_v<T, SetHeadPose>)
				{
					m_authored->tracking.head = value.value;
					remember(ControlChannel::Head);
				}
				else if constexpr (std::is_same_v<T, SetControllerPose>)
				{
					m_authored->tracking.controllers.at(value.hand) = value.value;
					remember(value.hand ? ControlChannel::RightPose : ControlChannel::LeftPose);
				}
				else if constexpr (std::is_same_v<T, SetBodyControls>)
				{
					m_authored->tracking.body = value.value;
					remember(ControlChannel::Body);
				}
				else if constexpr (std::is_same_v<T, SetGaze>)
				{
					m_authored->tracking.gaze = value.value;
					remember(ControlChannel::Gaze);
				}
				else if constexpr (std::is_same_v<T, SetTracker>)
				{
					m_authored->tracking.trackers.at(value.index) = value.value;
					entry.trackers.at(value.index) = true;
					entry.trackerIdentities.at(value.index) = value.value.identity;
					entry.trackerSources.at(value.index) = value.value.sourcePath;
					remember(ControlChannel::Trackers);
				}
				else if constexpr (std::is_same_v<T, SetTrackers>)
				{
					m_authored->tracking.trackers = value.value;
					entry.trackerCollection = true;
					remember(ControlChannel::Trackers);
				}
				else if constexpr (std::is_same_v<T, SetBodyProportions>)
				{
					m_authored->proportions = value.value;
					remember(ControlChannel::Proportions);
				}
			},
			command);
	entry.description = Describe(entry.before, *m_authored);
	m_open = edit.continuous;
}
void UndoStack::Clear()
{
	m_entries.clear();
	m_authored.reset();
	m_open = false;
}
std::vector<UndoStack::Change> UndoStack::Changes() const
{
	std::vector<Change> result;
	for (const auto& entry : m_entries)
		result.push_back({entry.label.empty() ? entry.description : entry.label, entry.time});
	return result;
}
std::string UndoStack::Describe(const State& before, const State& after)
{
	const auto moved = [](const XrPosef& a, const XrPosef& b)
	{
		const float distance =
			std::hypot(a.position.x - b.position.x, a.position.y - b.position.y, a.position.z - b.position.z);
		const float dot = a.orientation.x * b.orientation.x + a.orientation.y * b.orientation.y +
						  a.orientation.z * b.orientation.z + a.orientation.w * b.orientation.w;
		return distance > .0005f || std::abs(dot) < .99999f;
	};
	const auto movedOptional = [&](const std::optional<XrPosef>& a, const std::optional<XrPosef>& b)
	{
		return a.has_value() != b.has_value() || (a && moved(*a, *b));
	};
	const auto& from = before.tracking;
	const auto& to = after.tracking;
	// Moving the player moves everything that follows it; name the cause. The first rig edit only
	// starts sending a root pose, which moves nothing.
	if (from.body.rootPose && to.body.rootPose && moved(*from.body.rootPose, *to.body.rootPose))
		return "Player moved";
	if (from.body.preset != to.body.preset)
		return "Body pose changed";
	char text[64];
	const auto measure = [&](const char* name, float a, float b)
	{
		std::snprintf(text, sizeof(text), "%s %.2f \u2192 %.2f m", name, a, b);
		return std::string(text);
	};
	if (before.proportions.height != after.proportions.height)
		return measure("Height", before.proportions.height, after.proportions.height);
	if (before.proportions.shoulderWidth != after.proportions.shoulderWidth)
		return measure("Shoulder width", before.proportions.shoulderWidth, after.proportions.shoulderWidth);
	if (before.proportions.hipWidth != after.proportions.hipWidth)
		return measure("Hip width", before.proportions.hipWidth, after.proportions.hipWidth);
	std::vector<std::string> names;
	bool posed = false;
	const auto note = [&](std::string name, bool pose)
	{
		names.push_back(std::move(name));
		posed |= pose;
	};
	if (moved(from.head.pose, to.head.pose))
		note("Head", true);
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		const auto& a = from.controllers[hand];
		const auto& b = to.controllers[hand];
		const bool pose = moved(a.pose.pose, b.pose.pose);
		if (pose || a.connected != b.connected)
			note(hand ? "Right hand" : "Left hand", pose);
	}
	if (from.trackers.size() != to.trackers.size())
		note("Tracker inventory", false);
	for (std::size_t slot = 0; slot < std::min(from.trackers.size(), to.trackers.size()); ++slot)
	{
		const auto& a = from.trackers[slot];
		const auto& b = to.trackers[slot];
		const bool pose = a.anchor != b.anchor || movedOptional(a.poseOverride, b.poseOverride) ||
						  movedOptional(a.jointOffset, b.jointOffset);
		if (pose || a.poseFlags != b.poseFlags || a.identity != b.identity || a.sourcePath != b.sourcePath ||
			a.connected != b.connected || a.tracked != b.tracked || a.role != b.role)
			note("Tracker " + std::to_string(slot + 1), pose);
	}
	if (from.gaze.yaw != to.gaze.yaw || from.gaze.pitch != to.gaze.pitch || from.gaze.available != to.gaze.available ||
		from.gaze.tracked != to.gaze.tracked)
		note("Eye gaze", false);
	if (from.body.active != to.body.active || from.body.confidence != to.body.confidence)
		note("Body", false);
	const char* verb = posed ? " moved" : " changed";
	if (names.empty())
		return "No change";
	if (names.size() == 1)
		return names.front() + verb;
	if (names.size() == 2)
		return names[0] + " and " + names[1] + verb;
	return std::to_string(names.size()) + " devices" + verb;
}
std::optional<ManualTransaction> UndoStack::Undo(const ObservedSetup& current, std::optional<CommandSource> source,
												 std::size_t steps)
{
	if (m_entries.empty() || steps == 0)
		return {};
	steps = std::min(steps, m_entries.size());
	State restored = m_authored.value_or(State{current.values.tracking, current.values.proportions});
	std::array<bool, ControlChannelCount> channels{};
	std::array<bool, MaxTrackerDevices> trackers{};
	bool trackerCollection = false;
	// Walk backward so the oldest selected edit supplies each channel's original value.
	for (std::size_t index = m_entries.size(); index > m_entries.size() - steps; --index)
	{
		const auto& entry = m_entries[index - 1];
		const auto restore = [&](ControlChannel channel, auto& value, const auto& before)
		{
			const auto slot = static_cast<std::size_t>(channel);
			if (entry.channels[slot])
			{
				value = before;
				channels[slot] = true;
			}
		};
		restore(ControlChannel::Head, restored.tracking.head, entry.before.tracking.head);
		for (std::size_t hand = 0; hand < 2; ++hand)
			restore(hand ? ControlChannel::RightPose : ControlChannel::LeftPose, restored.tracking.controllers[hand],
					entry.before.tracking.controllers[hand]);
		restore(ControlChannel::Body, restored.tracking.body, entry.before.tracking.body);
		restore(ControlChannel::Gaze, restored.tracking.gaze, entry.before.tracking.gaze);
		restore(ControlChannel::Proportions, restored.proportions, entry.before.proportions);
		if (entry.trackerCollection)
		{
			restored.tracking.trackers = entry.before.tracking.trackers;
			trackerCollection = true;
			channels[static_cast<std::size_t>(ControlChannel::Trackers)] = true;
		}
		else
			for (std::size_t slot = 0; slot < trackers.size(); ++slot)
				if (entry.trackers[slot])
				{
					if (slot >= restored.tracking.trackers.size() || slot >= entry.before.tracking.trackers.size() ||
						restored.tracking.trackers[slot].identity != entry.trackerIdentities[slot] ||
						restored.tracking.trackers[slot].sourcePath != entry.trackerSources[slot])
						return {};
					restored.tracking.trackers[slot] = entry.before.tracking.trackers[slot];
					trackers[slot] = true;
					channels[static_cast<std::size_t>(ControlChannel::Trackers)] = true;
				}
	}
	for (std::size_t channel = 0; channel < channels.size(); ++channel)
		if (channels[channel] && current.owners[channel] && current.owners[channel] != source)
			return {};
	for (std::size_t scenario = 0; scenario < current.scenarios.size(); ++scenario)
		if (current.scenarios[scenario] &&
			channels[static_cast<std::size_t>(ScenarioInputChannel(static_cast<FailureScenario>(scenario)))])
			return {};
	if (channels[static_cast<std::size_t>(ControlChannel::Trackers)] && !current.trackerAttachmentsAvailable)
		return {};
	ManualTransaction result;
	const auto restore = [&](ControlChannel channel, SimulationCommand command)
	{
		if (channels[static_cast<std::size_t>(channel)])
		{
			result.commands.emplace_back(TakeControl{channel, false});
			result.commands.push_back(std::move(command));
		}
	};
	restore(ControlChannel::Head, SetHeadPose{restored.tracking.head});
	for (std::uint32_t hand = 0; hand < 2; ++hand)
		restore(hand ? ControlChannel::RightPose : ControlChannel::LeftPose,
				SetControllerPose{hand, restored.tracking.controllers[hand]});
	if (channels[static_cast<std::size_t>(ControlChannel::Body)] && !current.bodyRootAvailable)
		restored.tracking.body.rootPose.reset();
	restore(ControlChannel::Body, SetBodyControls{restored.tracking.body, restored.tracking.bodyObservation});
	restore(ControlChannel::Gaze, SetGaze{restored.tracking.gaze});
	if (channels[static_cast<std::size_t>(ControlChannel::Trackers)])
	{
		result.commands.emplace_back(TakeControl{ControlChannel::Trackers, false});
		if (trackerCollection)
			result.commands.emplace_back(SetTrackers{restored.tracking.trackers});
		else
			for (std::uint32_t slot = 0; slot < trackers.size(); ++slot)
				if (trackers[slot])
					result.commands.emplace_back(SetTracker{slot, restored.tracking.trackers[slot]});
	}
	restore(ControlChannel::Proportions, SetBodyProportions{restored.proportions});
	if (result.commands.empty())
		return {};
	m_entries.resize(m_entries.size() - steps);
	m_authored = std::move(restored);
	m_open = false;
	return result;
}
} // namespace simxr::app
