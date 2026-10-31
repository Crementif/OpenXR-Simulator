#include "runtime/SessionPublication.h"
#include "Pch.h"
#include "simulation/ControllerPoses.h"

namespace simxr
{
SessionPublication::PreparedStart::PreparedStart(SessionPublication& owner, RuntimeTime epoch, ViewLayout layout)
	: m_owner(owner), m_baseline(owner.m_initial.value_or(CommandConfiguration{})), m_epoch(epoch), m_layout(layout)
{
	owner.m_commands.Start(owner.m_seed, owner.m_overrideInitial ? owner.m_initial : std::nullopt);
}

SessionPublication::PreparedStart::~PreparedStart()
{
	if (!m_committed)
		m_owner.m_commands.Close();
}

const ReferenceSpaceState& SessionPublication::PreparedStart::ReferenceSpaces() const
{
	return m_baseline.referenceSpaces;
}

void SessionPublication::PreparedStart::Commit()
{
	if (m_committed)
		throw std::logic_error("Session publication start was already committed");
	m_owner.m_instance.Trackers().BeginSession();
	m_owner.m_tracking.Reset();
	m_owner.m_baseline = std::move(m_baseline);
	m_owner.m_epoch = m_epoch;
	m_owner.m_sessionOverride = false;
	m_owner.m_viewHistory.clear();
	m_owner.m_viewQueries.clear();
	m_owner.m_layout = m_layout;
	m_committed = true;
}

SessionPublication::FrameBoundary::FrameBoundary(SessionPublication& owner, bool focused,
												 const ReferenceSpaceState& spaces)
	: m_owner(owner), m_baseline(owner.m_baseline), m_epoch(owner.m_epoch)
{
	auto observed = owner.m_input.Snapshot();
	if (observed.revision != owner.m_managedInputRevision)
		m_baseline.input = std::move(observed);
	m_baseline.tracking.focused = owner.m_sessionOverride || focused;
	m_baseline.referenceSpaces = spaces;
}

void SessionPublication::FrameBoundary::Reserve(
	const FramePrediction& prediction, const std::function<void(const AppliedCommandSnapshot&)>& reserveHistory)
{
	if (m_applied)
		throw std::logic_error("Frame publication was already reserved");
	const SimulationTime time{prediction.displayTime.value - m_epoch.value};
	m_applied = m_owner.m_commands.Apply(
		prediction.id, time, prediction.displayTime, m_baseline, [&](const auto& applied) { reserveHistory(*applied); },
		LiveInputClock::now(), prediction.clockRevision);
}

const AppliedCommandSnapshot& SessionPublication::FrameBoundary::Applied() const
{
	if (!m_applied)
		throw std::logic_error("Frame publication was not reserved");
	return *m_applied;
}

std::optional<XrSessionState> SessionPublication::FrameBoundary::StateTransition(XrSessionState current)
{
	const auto& scenarios = Applied().scenarios;
	const auto active = [&](FailureScenario scenario) {
		return scenarios[static_cast<std::size_t>(scenario)];
	};
	std::optional<XrSessionState> transition;
	if (current != XR_SESSION_STATE_STOPPING && current != XR_SESSION_STATE_LOSS_PENDING)
	{
		if (active(FailureScenario::SessionLoss))
			transition = XR_SESSION_STATE_LOSS_PENDING;
		else if (active(FailureScenario::Hidden))
			transition = XR_SESSION_STATE_SYNCHRONIZED;
		else if (active(FailureScenario::Unfocused))
			transition = XR_SESSION_STATE_VISIBLE;
		else if (m_owner.m_sessionOverride)
			transition = XR_SESSION_STATE_FOCUSED;
	}
	m_owner.m_sessionOverride = active(FailureScenario::Unfocused) || active(FailureScenario::Hidden);
	return transition;
}

void SessionPublication::FrameBoundary::Publish()
{
	if (m_published)
		throw std::logic_error("Frame publication was already committed");
	const auto& applied = Applied();
	m_owner.m_baseline.input = std::move(m_baseline.input);
	m_owner.m_baseline.referenceSpaces = applied.configuration.referenceSpaces;
	m_owner.PublishEffective(applied.runtimeTime, applied.configuration);
	m_published = true;
}

SessionPublication::SessionPublication(Instance& instance, ActionSyncState& actions)
	: m_instance(instance), m_actions(actions), m_gaze(instance.Extensions())
{
	TrackingSnapshot initial;
	for (std::size_t index = 0; index < initial.trackers.size(); ++index)
		initial.trackers[index].device = initial.input.trackers[index];
	InputSnapshot input;
	m_gaze.ContributeInput(initial, input);
	m_instance.Trackers().ContributeInput(initial, input);
	if (XR_FAILED(m_input.Publish(std::move(input))))
		throw std::logic_error("Invalid initial device input");
	m_managedInputRevision = m_input.Revision();
}

void SessionPublication::Configure(std::uint64_t seed, std::optional<CommandConfiguration> initial,
								   bool overrideInitial)
{
	CommandScheduler validation;
	validation.Start(seed, initial);
	m_seed = seed;
	m_initial = std::move(initial);
	m_overrideInitial = overrideInitial;
}

void SessionPublication::Stop()
{
	m_commands.Close();
	m_input.ReleaseControls();
	m_managedInputRevision = m_input.Revision();
	for (auto& controls : m_baseline.input.controllers)
	{
		const bool connected = controls.connected;
		controls = {};
		controls.connected = connected;
	}
}

bool SessionPublication::ReleaseSource(CommandSource source, bool retire)
{
	return ReleaseChannels(m_commands.ReleaseSource(source, retire));
}

bool SessionPublication::ExpireLiveInput(LiveInputClock::time_point now)
{
	const auto channels = m_commands.ExpireLiveInput(now);
	return std::any_of(channels.begin(), channels.end(), [](bool owned) { return owned; }) && ReleaseChannels(channels);
}

void SessionPublication::StopPoseSequence(CommandSource source)
{
	ReleaseChannels(m_commands.StopPoseSequence(source));
}

void SessionPublication::PublishTracking(RuntimeTime time, TrackingState state, const ViewSettings& settings,
										 bool focused)
{
	state.focused = focused;
	auto baseline = m_baseline;
	baseline.tracking = std::move(state);
	baseline.views = settings;
	// Only native baseline values change after WaitFrame. Applied command
	// overlays remain authoritative and are not executed a second time.
	const auto effective = m_commands.Merge(baseline);
	PublishEffective(time, effective);
	m_baseline = std::move(baseline);
}

XrResult SessionPublication::PublishInput(InputSnapshot input)
{
	if (XR_FAILED(InputState::Validate(input)))
		return XR_ERROR_VALIDATION_FAILURE;
	auto baseline = m_baseline;
	baseline.input = input;
	auto merged = m_commands.Merge(baseline);
	auto result = m_input.Publish(std::move(merged.input));
	if (XR_SUCCEEDED(result))
	{
		m_baseline.input = std::move(input);
		m_managedInputRevision = m_input.Revision();
	}
	return result;
}

RecordingResult SessionPublication::StartRecording(const ReferenceSpaceState& spaces, bool drain, CommandSource owner)
{
	auto baseline = m_baseline;
	auto observed = m_input.Snapshot();
	if (observed.revision != m_managedInputRevision)
		baseline.input = std::move(observed);
	// Native publication may report the scenario-induced focus state. Preserve
	// the underlying focused origin rather than baking that failure into it.
	baseline.tracking.focused = m_sessionOverride || baseline.tracking.focused;
	return m_commands.StartRecording(baseline, spaces, drain, owner);
}

void SessionPublication::CaptureFrame(FrameId frame, FrameSubmissionState& submission) const
{
	submission.viewLayout = m_layout;
	for (const auto& query : m_viewQueries)
		if (query.displayTime == submission.displayTime)
			submission.viewQuery = query;
	submission.input = m_input.Snapshot();
	submission.actions = m_actions.Snapshot();
	submission.commands = m_commands.At(frame);
	submission.tracking = m_tracking.Sample(submission.displayTime);
	for (auto revision = m_viewHistory.rbegin(); revision != m_viewHistory.rend(); ++revision)
		if (revision->time <= submission.displayTime)
		{
			submission.viewSettings = revision->configuration.Settings();
			break;
		}
	if (submission.commands)
	{
		auto historical = std::make_shared<AppliedCommandSnapshot>(*submission.commands);
		historical->configuration.input = submission.input;
		historical->configuration.views = submission.viewSettings;
		historical->configurationAvailable = submission.tracking.has_value();
		if (submission.tracking)
		{
			historical->configuration.tracking = submission.tracking->input;
			historical->configuration.proportions = submission.tracking->proportions;
		}
		submission.commands = std::move(historical);
	}
}

void SessionPublication::RecordViewQuery(ViewQuerySnapshot query)
{
	std::erase_if(m_viewQueries, [&](const auto& previous) { return previous.displayTime == query.displayTime; });
	m_viewQueries.push_back(std::move(query));
	while (m_viewQueries.size() > 128)
		m_viewQueries.pop_front();
}

bool SessionPublication::TakeProfileChange() noexcept
{
	return std::exchange(m_profileChanged, false);
}

CommandConfiguration SessionPublication::ConfigurationSnapshot(const FramePrediction& lastIssued) const
{
	auto baseline = m_baseline;
	if (lastIssued.id.value || !m_initial)
		baseline.input = m_input.Snapshot();
	auto result = m_commands.Merge(baseline);
	if (const auto sample = m_tracking.Sample(lastIssued.displayTime))
	{
		result.tracking = sample->input;
		result.proportions = sample->proportions;
	}
	return result;
}

CommandScheduler& SessionPublication::Commands()
{
	return m_commands;
}

InputState& SessionPublication::Input()
{
	return m_input;
}

TrackingSystem& SessionPublication::Tracking()
{
	return m_tracking;
}

ViewLayout SessionPublication::Layout() const noexcept
{
	return m_layout;
}

ViewConfiguration SessionPublication::ConfigurationAt(RuntimeTime time) const
{
	for (auto revision = m_viewHistory.rbegin(); revision != m_viewHistory.rend(); ++revision)
		if (revision->time <= time)
			return revision->configuration;
	return ViewConfiguration{m_layout};
}

LocatedViews SessionPublication::ViewsAt(RuntimeTime time, bool foveated) const
{
	auto sample = m_tracking.Sample(time);
	if (!sample)
		sample.emplace();
	return ConfigurationAt(time).Locate(*sample, foveated);
}

PoseSample SessionPublication::ActionPose(XrAction action, XrPath subaction, RuntimeTime time) const
{
	std::vector<ResolvedActionSource> sources;
	if (XR_FAILED(m_actions.SpaceSources(action, subaction, sources)))
		return {};
	auto sample = m_tracking.Sample(time);
	if (!sample)
		return {};
	for (const auto& source : sources)
	{
		if (!source.active)
			continue;
		switch (source.device.kind)
		{
		case InputDeviceKind::Controller:
			if (source.device.value < sample->controllers.size())
			{
				const auto& controller = sample->controllers[source.device.value];
				if (source.binding.component != "/input/aim/pose")
					return controller.pose;
				if (controller.aim)
					return *controller.aim;
				return ControllerAimFromGrip(controller.pose,
											 m_instance.Actions().PathString(source.binding.profile).value_or(""),
											 source.device.value == 0);
			}
			break;
		case InputDeviceKind::EyeGaze:
			return m_gaze.Pose(*sample);
		case InputDeviceKind::Tracker:
			for (const auto& tracker : sample->trackers)
				if (tracker.device.identity.value == source.device.value)
					return tracker.pose;
			break;
		}
	}
	return {};
}

XrResult SessionPublication::ValidatePoseAction(XrAction action, XrPath subaction) const
{
	auto definition = m_instance.Actions().Action(action);
	if (!definition)
		return XR_ERROR_HANDLE_INVALID;
	if (definition->type != XR_ACTION_TYPE_POSE_INPUT)
		return XR_ERROR_ACTION_TYPE_MISMATCH;
	return m_instance.Actions().ValidateSubaction(*definition, subaction);
}

bool SessionPublication::ReleaseChannels(const std::array<bool, ControlChannelCount>& channels)
{
	const std::array<bool, 2> released{channels[static_cast<std::size_t>(ControlChannel::LeftControls)],
									   channels[static_cast<std::size_t>(ControlChannel::RightControls)]};
	m_input.ReleaseControls(released);
	m_managedInputRevision = m_input.Revision();
	const bool restoreFocus = channels[static_cast<std::size_t>(ControlChannel::Session)] && m_sessionOverride;
	if (restoreFocus)
		m_sessionOverride = false;
	for (std::size_t hand = 0; hand < released.size(); ++hand)
		if (released[hand])
		{
			const bool connected = m_baseline.input.controllers[hand].connected;
			m_baseline.input.controllers[hand] = {};
			m_baseline.input.controllers[hand].connected = connected;
		}
	return restoreFocus;
}

void SessionPublication::PublishEffective(RuntimeTime time, const CommandConfiguration& effective)
{
	ViewConfiguration configuration(m_layout, effective.views);
	m_tracking.SetBodyProportions(effective.proportions);
	const auto snapshot = m_tracking.Publish(time, effective.tracking, true);
	auto input = effective.input;
	input.time = std::max({input.time, time, m_input.Snapshot().time});
	m_gaze.ContributeInput(snapshot, input);
	m_instance.Trackers().ContributeInput(snapshot, input);
	if (XR_FAILED(m_input.Publish(input)))
		throw std::logic_error("Invalid device input");
	m_managedInputRevision = m_input.Revision();
	std::vector<XrPath> changedPaths;
	const auto trackerResult = m_instance.Trackers().Publish(snapshot, &changedPaths);
	if (XR_FAILED(trackerResult) && trackerResult != XR_ERROR_FUNCTION_UNSUPPORTED)
		throw std::logic_error("Invalid tracker inventory");
	if (!changedPaths.empty())
	{
		XrPath trackerProfile{};
		m_instance.Actions().StringToPath("/interaction_profiles/htc/vive_tracker_htcx", trackerProfile);
		for (const auto& action : m_actions.Snapshot().actions)
			for (const auto& binding : m_instance.Actions().Bindings(action.definition.handle, trackerProfile))
				if (std::find(changedPaths.begin(), changedPaths.end(), binding.userPath) != changedPaths.end())
					m_profileChanged = true;
	}
	if (!m_viewHistory.empty() && m_viewHistory.back().time == time)
		m_viewHistory.back().configuration = std::move(configuration);
	else
		m_viewHistory.push_back({time, std::move(configuration)});
	while (m_viewHistory.size() > 128)
		m_viewHistory.pop_front();
}

} // namespace simxr
