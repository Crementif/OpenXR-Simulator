#include "simulation/CommandScheduler.h"
#include "Pch.h"

namespace simxr
{
// Canonical words compare retry payloads exactly, including float bits. They are
// an internal identity representation, not a wire format or an object-memory dump.
class CommandIdentityBuilder
{
public:
	template<class T>
	void Add(T value)
	{
		m_words.push_back(static_cast<std::uint64_t>(value));
	}

	void Add(float value)
	{
		Add(std::bit_cast<std::uint32_t>(value));
	}

	void Add(const XrVector3f& value)
	{
		Add(value.x);
		Add(value.y);
		Add(value.z);
	}

	void Add(const XrPosef& value)
	{
		Add(value.orientation.x);
		Add(value.orientation.y);
		Add(value.orientation.z);
		Add(value.orientation.w);
		Add(value.position);
	}

	void Add(const PoseSample& value)
	{
		Add(value.pose);
		Add(value.flags);
		Add(value.sampleTime.value.count());
		Add(value.eyeGazeSampleTime.has_value());
		if (value.eyeGazeSampleTime)
			Add(value.eyeGazeSampleTime->value.count());
	}

	void Add(const std::vector<TrackerDevice>& value)
	{
		Add(value.size());
		for (const auto& tracker : value)
			Add(tracker);
	}

	void Add(const ControllerPose& value)
	{
		Add(value.pose);
		Add(value.connected);
		Add(value.aim.has_value());
		if (value.aim)
			Add(*value.aim);
	}

	void Add(const ControllerControls& value)
	{
		Add(value.physicalProfile.size());
		for (unsigned char character : value.physicalProfile)
			Add(character);
		Add(value.triggerClick.has_value());
		if (value.triggerClick)
			Add(*value.triggerClick);
		for (float component : {value.trigger, value.grip, value.gripForce, value.stick.x, value.stick.y,
								value.trackpad.x, value.trackpad.y, value.trackpadForce})
			Add(component);
		for (bool flag : {value.primary, value.secondary, value.menu, value.back, value.thumbClick, value.trackpadClick,
						  value.primaryTouch, value.secondaryTouch, value.triggerTouch, value.thumbTouch,
						  value.trackpadTouch, value.thumbrestTouch, value.connected})
			Add(flag);
	}

	void Add(const BodyControls& value)
	{
		Add(value.active);
		Add(value.confidence);
		Add(value.preset);
		Add(value.rootPose.has_value());
		if (value.rootPose)
			Add(*value.rootPose);
		for (const auto& control : value.controls)
		{
			Add(control.tracked);
			Add(control.offset.has_value());
			if (control.offset)
				Add(*control.offset);
		}
		for (const auto& joint : value.joints)
		{
			Add(joint.has_value());
			if (joint)
			{
				Add(joint->tracked);
				Add(joint->pose.has_value());
				if (joint->pose)
					Add(*joint->pose);
			}
		}
	}

	void Add(const std::optional<BodyObservation>& value)
	{
		Add(value.has_value());
		if (!value)
			return;
		Add(value->jointSet);
		Add(value->requestedTime.value.count());
		Add(value->returnedTime.has_value());
		if (value->returnedTime)
			Add(value->returnedTime->value.count());
		Add(value->located);
		Add(value->active.has_value());
		if (value->active)
			Add(*value->active);
		Add(value->allJointPosesTracked.has_value());
		if (value->allJointPosesTracked)
			Add(*value->allJointPosesTracked);
		Add(value->confidence.has_value());
		if (value->confidence)
			Add(*value->confidence);
		Add(value->skeletonRevision.has_value());
		if (value->skeletonRevision)
			Add(*value->skeletonRevision);
		for (const auto& joint : value->joints)
		{
			Add(joint.has_value());
			if (joint)
			{
				Add(joint->pose);
				Add(joint->flags);
			}
		}
		Add(value->skeleton.has_value());
		if (value->skeleton)
			for (const auto& joint : *value->skeleton)
			{
				Add(joint.has_value());
				if (!joint)
					continue;
				Add(joint->joint);
				Add(joint->parent.has_value());
				if (joint->parent)
					Add(*joint->parent);
				Add(joint->bindPose);
			}
	}

	void Add(const GazeInput& value)
	{
		Add(value.yaw);
		Add(value.pitch);
		Add(value.available);
		Add(value.tracked);
	}

	void Add(const TrackerDevice& value)
	{
		Add(value.identity.value);
		Add(value.poseFlags);
		Add(value.sourcePath.View().size());
		for (const unsigned char character : value.sourcePath.View())
			Add(character);
		Add(value.role);
		Add(value.connected);
		Add(value.tracked);
		Add(value.anchor.has_value());
		if (value.anchor)
			Add(*value.anchor);
		Add(value.jointOffset.has_value());
		if (value.jointOffset)
			Add(*value.jointOffset);
		Add(value.poseOverride.has_value());
		if (value.poseOverride)
			Add(*value.poseOverride);
	}

	void Add(const std::optional<StereoEyeViews>& value)
	{
		Add(value.has_value());
		if (!value)
			return;
		Add(value->spaceRevision);
		for (std::size_t eye = 0; eye < 2; ++eye)
		{
			Add(value->poses[eye]);
			const auto& fov = value->fovs[eye];
			Add(fov.angleLeft);
			Add(fov.angleRight);
			Add(fov.angleUp);
			Add(fov.angleDown);
		}
	}

	void Add(const ViewSettings& value)
	{
		Add(value.ipd);
		Add(value.outerWidth);
		Add(value.outerHeight);
		Add(value.insetWidth);
		Add(value.insetHeight);
		Add(value.insetHalfAngle);
		for (const auto& component : value.outerFov)
		{
			Add(component.angleLeft);
			Add(component.angleRight);
			Add(component.angleUp);
			Add(component.angleDown);
		}
	}

	void Add(const BodyProportions& value)
	{
		Add(value.height);
		Add(value.shoulderWidth);
		Add(value.hipWidth);
	}

	void Add(const ReferenceSpaceState& value)
	{
		Add(value.local);
		Add(value.stage);
		Add(value.stageBounds.has_value());
		if (value.stageBounds)
		{
			Add(value.stageBounds->width);
			Add(value.stageBounds->height);
		}
	}

	std::vector<std::uint64_t> Finish() &&
	{
		return std::move(m_words);
	}

private:
	std::vector<std::uint64_t> m_words;
};

static std::vector<std::uint64_t> RequestWords(const CommandTransaction& transaction)
{
	CommandIdentityBuilder identity;
	identity.Add(transaction.source.kind);
	identity.Add(transaction.source.id);
	identity.Add(transaction.id);
	identity.Add(transaction.latePolicy);
	identity.Add(transaction.target.frame.has_value());
	if (transaction.target.frame)
		identity.Add(transaction.target.frame->value);
	identity.Add(transaction.target.time.has_value());
	if (transaction.target.time)
		identity.Add(transaction.target.time->value.count());
	for (const auto* group : {&transaction.commands, &transaction.expected})
	{
		identity.Add(group->size());
		for (const auto& command : *group)
		{
			identity.Add(command.index());
			std::visit(
				[&](const auto& value) {
					using T = std::decay_t<decltype(value)>;
					if constexpr (std::is_same_v<T, TakeControl>)
					{
						identity.Add(value.channel);
						identity.Add(value.replace);
					}
					else if constexpr (std::is_same_v<T, ReleaseControl>)
						identity.Add(value.channel);
					else if constexpr (std::is_same_v<T, SetScenario>)
					{
						identity.Add(value.scenario);
						identity.Add(value.enabled);
					}
					else if constexpr (std::is_same_v<T, SetControllerProfile>)
					{
						identity.Add(value.hand);
						identity.Add(value.path.size());
						for (unsigned char c : value.path)
							identity.Add(c);
					}
					else
					{
						if constexpr (std::is_same_v<T, SetControllerPose> || std::is_same_v<T, SetControllerControls>)
							identity.Add(value.hand);
						if constexpr (std::is_same_v<T, SetTracker>)
							identity.Add(value.index);
						identity.Add(value.value);
						if constexpr (std::is_same_v<T, SetBodyControls>)
						{
							identity.Add(value.observation);
							identity.Add(value.observationOrigin.has_value());
							if (value.observationOrigin)
								identity.Add(value.observationOrigin->value.count());
						}
					}
				},
				command);
		}
	}
	return std::move(identity).Finish();
}

static bool ExpectedInputsMatch(const CommandState& state, const CommandConfiguration& baseline, RuntimeTime time,
								const std::vector<SimulationCommand>& expected)
{
	if (expected.empty())
		return true;
	AppliedCommandSnapshot snapshot;
	state.Capture(snapshot, baseline, time);
	CommandTransaction wanted, actual;
	for (const auto& input : expected)
	{
		if (const auto* head = std::get_if<SetHeadPose>(&input))
		{
			auto expectedPose = head->value;
			auto currentPose = snapshot.configuration.tracking.head;
			expectedPose.sampleTime = currentPose.sampleTime = {};
			expectedPose.eyeGazeSampleTime.reset();
			currentPose.eyeGazeSampleTime.reset();
			wanted.commands.push_back(SetHeadPose{expectedPose});
			actual.commands.push_back(SetHeadPose{currentPose});
		}
		else if (const auto* controls = std::get_if<SetControllerControls>(&input))
		{
			if (controls->hand >= snapshot.configuration.input.controllers.size())
				return false;
			wanted.commands.push_back(*controls);
			actual.commands.push_back(
				SetControllerControls{controls->hand, snapshot.configuration.input.controllers[controls->hand]});
		}
		else if (const auto* views = std::get_if<SetViewSettings>(&input))
		{
			wanted.commands.push_back(*views);
			actual.commands.push_back(SetViewSettings{snapshot.configuration.views});
		}
		else
			return false;
	}
	return RequestWords(wanted) == RequestWords(actual);
}

static std::optional<CommandOutcome> ReleaseOutcome(CommandSource source,
													const std::array<bool, ControlChannelCount>& channels,
													FrameId frame, SimulationTime time, bool afterFrame)
{
	const auto count = std::count(channels.begin(), channels.end(), true);
	if (!count)
		return {};
	CommandOutcome outcome;
	outcome.result = CommandResult::Released;
	outcome.transaction.source = source;
	outcome.appliedFrame = frame;
	outcome.appliedTime = time;
	outcome.afterFrame = afterFrame;
	outcome.transaction.commands.reserve(count);
	for (std::size_t index = 0; index < channels.size(); ++index)
		if (channels[index])
			outcome.transaction.commands.push_back(ReleaseControl{static_cast<ControlChannel>(index)});
	return outcome;
}

static std::shared_ptr<const CommandRecordingOrigin> ReleaseState(const CommandState& state,
																  const CommandConfiguration& baseline, FrameId frame,
																  SimulationTime time, RuntimeTime runtimeTime)
{
	auto origin = state.RecordingOrigin(baseline, runtimeTime);
	origin.frame = frame;
	origin.time = time;
	origin.runtimeTime = runtimeTime;
	return std::make_shared<const CommandRecordingOrigin>(std::move(origin));
}

static bool HasRelease(const CommandOutcome& outcome)
{
	return outcome.result == CommandResult::Applied &&
		   std::any_of(outcome.transaction.commands.begin(), outcome.transaction.commands.end(),
					   [](const auto& command) { return std::holds_alternative<ReleaseControl>(command); });
}

static bool ValidSource(CommandSource source)
{
	return source.id && (source.kind == CommandSourceKind::Manual || source.kind == CommandSourceKind::Mcp ||
						 source.kind == CommandSourceKind::Rig || source.kind == CommandSourceKind::Device);
}

class CommandScheduler::PreparedCommandBoundary
{
public:
	PreparedCommandBoundary(const CommandScheduler& scheduler, FrameId frame, SimulationTime time,
							RuntimeTime runtimeTime, const CommandConfiguration& baseline,
							LiveInputClock::time_point now, std::optional<std::uint64_t> clockRevision)
		: m_state(scheduler.m_state), m_baseline(baseline), m_pending(scheduler.m_pending),
		  m_outcomes(scheduler.m_outcomes), m_snapshots(scheduler.m_applied),
		  m_snapshot(std::make_shared<AppliedCommandSnapshot>()), m_historyCapacity(scheduler.m_historyCapacity),
		  m_sequence(scheduler.m_sequence), m_replacedSequence(scheduler.m_replacedSequence),
		  m_liveInput(scheduler.m_liveInput)
	{
		if (scheduler.m_revision == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Command revisions exhausted");
		m_snapshot->seed = scheduler.m_seed;
		m_snapshot->revision = scheduler.m_revision + 1;
		m_snapshot->frame = frame;
		m_snapshot->time = time;
		m_snapshot->runtimeTime = runtimeTime;
		m_state.BeginBoundary(baseline.referenceSpaces);
		for (auto command = m_pending.begin(); command != m_pending.end();)
		{
			if (command->result == CommandResult::Cancelled)
			{
				AppendHistory(*command);
				command = m_pending.erase(command);
				continue;
			}
			const auto target = command->effectiveTarget.value_or(command->transaction.target);
			if ((target.frame && *target.frame > frame) || (target.time && *target.time > time))
			{
				++command;
				continue;
			}
			auto outcome = *command;
			outcome.appliedFrame = frame;
			outcome.appliedTime = time;
			outcome.appliedRuntimeTime = runtimeTime;
			outcome.result = CommandResult::Applied;
			if (target.frame && *target.frame < frame)
			{
				if (outcome.transaction.latePolicy == CommandLatePolicy::Reject)
					outcome.result = CommandResult::Late;
				else
					outcome.effectiveTarget = CommandTarget{frame, {}};
			}
			if (outcome.result == CommandResult::Applied)
				outcome.result = ExpectedInputsMatch(m_state, baseline, runtimeTime, outcome.transaction.expected)
									 ? m_state.Apply(outcome.transaction, baseline, runtimeTime)
									 : CommandResult::Conflict;
			if (HasRelease(outcome))
				outcome.releaseState = ReleaseState(m_state, baseline, frame, time, runtimeTime);
			m_snapshot->outcomes.push_back(outcome);
			AppendHistory(std::move(outcome));
			command = m_pending.erase(command);
		}
		ApplyLiveInput(frame, time, runtimeTime, baseline, now, clockRevision);
		ApplySequence(frame, time, runtimeTime, baseline);
		m_state.Capture(*m_snapshot, baseline, runtimeTime);
		m_snapshots.push_back(m_snapshot);
		if (m_snapshots.size() > 128)
			m_snapshots.pop_front();
	}

	std::shared_ptr<const AppliedCommandSnapshot> Snapshot() const noexcept
	{
		return m_snapshot;
	}

	const std::vector<CommandOutcome>& Outcomes() const noexcept
	{
		return m_snapshot->outcomes;
	}

	void Commit(CommandScheduler& scheduler) noexcept
	{
		static_assert(std::is_nothrow_swappable_v<std::optional<PoseSequenceExecution>>);
		static_assert(std::is_nothrow_swappable_v<std::optional<CommandConfiguration>>);
		m_state.Swap(scheduler.m_state);
		m_baseline.swap(scheduler.m_baseline);
		m_pending.swap(scheduler.m_pending);
		m_outcomes.swap(scheduler.m_outcomes);
		m_snapshots.swap(scheduler.m_applied);
		scheduler.m_revision = m_snapshot->revision;
		scheduler.m_sequence.swap(m_sequence);
		scheduler.m_replacedSequence.swap(m_replacedSequence);
		std::swap(scheduler.m_liveInput, m_liveInput);
	}

	void MarkRecordingOverrun(bool overrun) noexcept
	{
		for (auto& outcome : m_snapshot->outcomes)
			outcome.recordingOverrun = overrun;
		for (auto& outcome : m_outcomes)
		{
			if (outcome.appliedFrame == m_snapshot->frame)
				outcome.recordingOverrun = overrun;
		}
	}

private:
	void AppendHistory(CommandOutcome outcome)
	{
		m_outcomes.push_back(std::move(outcome));
		if (m_outcomes.size() > m_historyCapacity)
			m_outcomes.pop_front();
	}

	void ReleaseSource(CommandSource source)
	{
		const auto channels = m_state.ReleaseSource(source);
		if (auto outcome = ReleaseOutcome(source, channels, m_snapshot->frame, m_snapshot->time, false))
		{
			outcome->appliedRuntimeTime = m_snapshot->runtimeTime;
			outcome->releaseState =
				ReleaseState(m_state, *m_baseline, m_snapshot->frame, m_snapshot->time, m_snapshot->runtimeTime);
			m_snapshot->outcomes.push_back(std::move(*outcome));
		}
	}

	void ApplyLiveInput(FrameId frame, SimulationTime time, RuntimeTime runtimeTime,
						const CommandConfiguration& baseline, LiveInputClock::time_point now,
						std::optional<std::uint64_t> clockRevision)
	{
		const auto owner = m_liveInput.Status().owner;
		if (!owner)
			return;
		for (const auto& outcome : m_snapshot->outcomes)
			if (outcome.transaction.source == *owner && outcome.result == CommandResult::Applied)
				for (const auto& command : outcome.transaction.commands)
					if (const auto* release = std::get_if<ReleaseControl>(&command);
						release &&
						(static_cast<std::size_t>(release->channel) < 5 || release->channel == ControlChannel::Views ||
						 release->channel == ControlChannel::Trackers || release->channel == ControlChannel::Body))
					{
						m_liveInput.Reset();
						return;
					}
		auto outcome = m_liveInput.Prepare(frame, time, m_state.OwnedChannels(*owner), now, clockRevision);
		if (!outcome)
			return;
		outcome->appliedRuntimeTime = runtimeTime;
		outcome->result = m_state.Apply(outcome->transaction, baseline, runtimeTime);
		if (HasRelease(*outcome))
			outcome->releaseState = ReleaseState(m_state, baseline, frame, time, runtimeTime);
		if (outcome->result != CommandResult::Applied)
			m_liveInput.Reset();
		m_snapshot->outcomes.push_back(*outcome);
		AppendHistory(std::move(*outcome));
	}

	void ApplySequence(FrameId frame, SimulationTime time, RuntimeTime runtimeTime,
					   const CommandConfiguration& baseline)
	{
		if (!m_sequence || !m_sequence->CanPrepare())
			return;
		if (m_sequence->Status().origin.value && !m_sequence->HasControl(m_state.OwnedChannels(m_sequence->Source())))
		{
			m_sequence->Accept(frame, CommandResult::Conflict);
			ReleaseSource(m_sequence->Source());
			return;
		}
		const auto initial = m_state.Merge(baseline);
		if (m_replacedSequence)
		{
			ReleaseSource(*m_replacedSequence);
			m_replacedSequence.reset();
		}
		PoseSequenceFrame prepared;
		try
		{
			prepared = m_sequence->Prepare(frame, initial);
		}
		catch (const std::invalid_argument&)
		{
			m_sequence->Accept(frame, CommandResult::Invalid);
		}
		catch (const std::overflow_error&)
		{
			m_sequence->Accept(frame, CommandResult::Invalid);
		}
		if (!m_sequence->CanPrepare())
		{
			ReleaseSource(m_sequence->Source());
			return;
		}
		if (!prepared.advanced)
			return;
		CommandResult result = CommandResult::Applied;
		if (!prepared.commands.empty())
		{
			const auto owned = m_state.OwnedChannels(m_sequence->Status().owner);
			for (auto& command : prepared.commands)
				if (auto* take = std::get_if<TakeControl>(&command))
					take->replace = owned[static_cast<std::size_t>(take->channel)];
			CommandOutcome outcome;
			outcome.transaction = {m_sequence->Status().groups + 1,
								   m_sequence->Source(),
								   {frame, {}},
								   CommandLatePolicy::Reject,
								   std::move(prepared.commands)};
			outcome.appliedFrame = frame;
			outcome.appliedTime = time;
			outcome.appliedRuntimeTime = runtimeTime;
			result = outcome.result = m_state.Apply(outcome.transaction, baseline, runtimeTime);
			if (HasRelease(outcome))
				outcome.releaseState = ReleaseState(m_state, baseline, frame, time, runtimeTime);
			m_snapshot->outcomes.push_back(outcome);
			AppendHistory(std::move(outcome));
		}
		m_sequence->Accept(frame, result);
		if (result != CommandResult::Applied)
			ReleaseSource(m_sequence->Source());
		else if (prepared.capture)
			m_snapshot->sequenceCapture =
				SequenceCaptureIntent{m_sequence->Status().id, frame, m_sequence->Status().owner};
	}

	CommandState m_state;
	std::optional<CommandConfiguration> m_baseline;
	std::deque<CommandOutcome> m_pending;
	std::deque<CommandOutcome> m_outcomes;
	std::deque<std::shared_ptr<const AppliedCommandSnapshot>> m_snapshots;
	std::shared_ptr<AppliedCommandSnapshot> m_snapshot;
	std::size_t m_historyCapacity;
	std::optional<PoseSequenceExecution> m_sequence;
	std::optional<CommandSource> m_replacedSequence;
	LiveInputSlot m_liveInput;
};

CommandScheduler::CommandScheduler(std::size_t pendingCapacity, std::size_t historyCapacity)
	: m_pendingCapacity(pendingCapacity), m_historyCapacity(historyCapacity)
{
	if (!pendingCapacity || pendingCapacity > 256 || !historyCapacity || historyCapacity > 1024)
		throw std::invalid_argument("Invalid command scheduler capacity");
	m_sources.reserve(64);
}

void CommandScheduler::Start(std::uint64_t seed, std::optional<CommandConfiguration> initial)
{
	CommandState prepared(std::move(initial));
	std::optional<PoseSequenceExecution> retired;
	std::lock_guard lock(m_mutex);
	m_state.Swap(prepared);
	m_baseline.reset();
	m_seed = seed;
	m_revision = 0;
	m_closed = false;
	m_pending.clear();
	m_outcomes.clear();
	m_sources.clear();
	m_applied.clear();
	m_liveInput.Reset();
	retired.swap(m_sequence);
	m_replacedSequence.reset();
	m_recorder.Stop();
	m_recordingOwner.reset();
}

void CommandScheduler::Close()
{
	std::shared_ptr<const PoseSequence> retired;
	std::lock_guard lock(m_mutex);
	if (m_sequence)
		retired = m_sequence->Program();
	// Teardown does not allocate or claim that previously applied work was undone.
	for (auto& pending : m_pending)
		pending.result = CommandResult::Cancelled;
	m_state.ClearOwnership();
	m_liveInput.Reset();
	if (m_sequence)
		m_sequence->Cancel();
	m_replacedSequence.reset();
	m_closed = true;
	m_recorder.Stop();
	m_recordingOwner.reset();
}

CommandResult CommandScheduler::SubmitLiveInput(CommandSource source, const LiveInputSample& sample,
												LiveInputClock::time_point now)
{
	std::lock_guard lock(m_mutex);
	if (m_closed)
		return CommandResult::Closed;
	if (const auto owner = m_liveInput.Status().owner;
		owner && !LiveInputSlot::HasChannels(m_state.OwnedChannels(*owner)))
		m_liveInput.Reset();
	return m_liveInput.Write(source, sample, m_state.OwnedChannels(source), now);
}

void CommandScheduler::AcknowledgeLiveInput()
{
	std::lock_guard lock(m_mutex);
	m_liveInput.AcknowledgeControls();
}

LiveInputStatus CommandScheduler::LiveInputState() const
{
	std::lock_guard lock(m_mutex);
	return m_liveInput.Status();
}

CommandOutcome CommandScheduler::Submit(CommandTransaction transaction)
{
	CommandOutcome outcome{std::move(transaction), CommandResult::Queued};
	std::lock_guard lock(m_mutex);
	if (m_closed)
	{
		outcome.result = CommandResult::Closed;
		return outcome;
	}
	auto& request = outcome.transaction;
	if (!request.id || !ValidSource(request.source) || request.commands.empty() || request.commands.size() > 64 ||
		request.target.frame.has_value() == request.target.time.has_value() || request.expected.size() > 3 ||
		(request.target.frame && !request.target.frame->value) ||
		(request.target.time && request.target.time->value.count() < 0) ||
		(request.latePolicy != CommandLatePolicy::Reject && request.latePolicy != CommandLatePolicy::RescheduleNext))
	{
		outcome.result = CommandResult::Invalid;
		return outcome;
	}
	// Variable-sized command data is bounded before retaining or fingerprinting it.
	for (const auto& input : request.expected)
	{
		if (const auto* controls = std::get_if<SetControllerControls>(&input))
		{
			if (controls->hand >= 2 || controls->value.physicalProfile.size() > 255)
			{
				outcome.result = CommandResult::Invalid;
				return outcome;
			}
		}
		else if (!std::holds_alternative<SetHeadPose>(input) && !std::holds_alternative<SetViewSettings>(input))
		{
			outcome.result = CommandResult::Invalid;
			return outcome;
		}
	}
	for (const auto& command : request.commands)
		if (const auto* profile = std::get_if<SetControllerProfile>(&command); profile && profile->path.size() > 255)
		{
			outcome.result = CommandResult::Invalid;
			return outcome;
		}
	const auto same = [&](const auto& value) {
		return !value.liveInput && value.transaction.source.kind != CommandSourceKind::Sequence &&
			   value.transaction.source.id == request.source.id && value.transaction.id == request.id;
	};
	for (const auto* records : {&m_pending, &m_outcomes})
		if (const auto found = std::find_if(records->begin(), records->end(), same); found != records->end())
		{
			// IDs identify immutable requests; changed payload retry is rejected.
			// Compare explicit scalar fields, never padding or native pointers.
			if (found->transaction.source != request.source ||
				RequestWords(found->requested ? *found->requested : found->transaction) != RequestWords(request))
				outcome.result = CommandResult::Conflict;
			else
				return *found;
			return outcome;
		}
	auto cursor = std::find_if(m_sources.begin(), m_sources.end(),
							   [&](const auto& source) { return source.source.id == request.source.id; });
	if (cursor != m_sources.end() && (cursor->source != request.source || cursor->last >= request.id))
	{
		outcome.result = CommandResult::Conflict;
		return outcome;
	}
	if ((cursor == m_sources.end() && m_sources.size() == 64) || m_pending.size() == m_pendingCapacity)
	{
		outcome.result = CommandResult::Capacity;
		return outcome;
	}
	if (!m_applied.empty())
	{
		const auto& last = *m_applied.back();
		if ((request.target.frame && *request.target.frame <= last.frame) ||
			(request.target.time && *request.target.time <= last.time))
		{
			if (request.latePolicy == CommandLatePolicy::Reject ||
				last.frame.value == std::numeric_limits<std::uint64_t>::max())
				outcome.result = CommandResult::Late;
			else
			{
				outcome.effectiveTarget = CommandTarget{FrameId{last.frame.value + 1}, {}};
				outcome.result = CommandResult::Rescheduled;
			}
		}
	}
	// Prepare all copies before publishing admission or consuming an ID.
	outcome.requested = std::make_shared<const CommandTransaction>(request);
	auto returned = outcome;
	if (outcome.result == CommandResult::Late)
	{
		auto outcomes = m_outcomes;
		outcomes.push_back(outcome);
		if (outcomes.size() > m_historyCapacity)
			outcomes.pop_front();
		returned.recordingOverrun = !m_recorder.Observe(outcome);
		outcomes.back().recordingOverrun = returned.recordingOverrun;
		m_outcomes.swap(outcomes);
	}
	else
		m_pending.push_back(std::move(outcome));
	if (cursor == m_sources.end())
		m_sources.push_back({returned.transaction.source, returned.transaction.id});
	else
		cursor->last = returned.transaction.id;
	return returned;
}

std::optional<CommandOutcome> CommandScheduler::Read(CommandSource source, std::uint64_t id) const
{
	std::lock_guard lock(m_mutex);
	for (const auto* records : {&m_pending, &m_outcomes})
		for (const auto& value : *records)
			if (value.transaction.source == source && value.transaction.id == id)
				return value;
	return {};
}

CommandSchedulerSnapshot CommandScheduler::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return {m_closed,
			m_closed ? 0
					 : static_cast<std::size_t>(
						   std::count_if(m_pending.begin(), m_pending.end(),
										 [](const auto& value) { return value.result != CommandResult::Cancelled; })),
			{m_outcomes.begin(), m_outcomes.end()},
			m_applied.empty() ? nullptr : m_applied.back(),
			m_seed};
}

CommandResult CommandScheduler::Cancel(CommandSource source, std::uint64_t id)
{
	std::lock_guard lock(m_mutex);
	const auto found = std::find_if(m_pending.begin(), m_pending.end(), [&](const auto& value) {
		return value.transaction.source == source && value.transaction.id == id;
	});
	if (found == m_pending.end())
		return CommandResult::NotFound;
	auto cancelled = *found;
	cancelled.result = CommandResult::Cancelled;
	if (!m_applied.empty())
	{
		cancelled.appliedFrame = m_applied.back()->frame;
		cancelled.appliedTime = m_applied.back()->time;
	}
	auto outcomes = m_outcomes;
	outcomes.push_back(cancelled);
	if (outcomes.size() > m_historyCapacity)
		outcomes.pop_front();
	outcomes.back().recordingOverrun = !m_recorder.Observe(cancelled);
	m_outcomes.swap(outcomes);
	m_pending.erase(found);
	return CommandResult::Cancelled;
}

std::array<bool, ControlChannelCount> CommandScheduler::ReleaseSource(CommandSource source, bool retire)
{
	std::shared_ptr<const PoseSequence> retired;
	std::lock_guard lock(m_mutex);
	if (m_sequence)
		retired = m_sequence->Program();
	return ReleaseSourceCommandsLocked(source, retire);
}

std::array<bool, ControlChannelCount> CommandScheduler::ExpireLiveInput(LiveInputClock::time_point now)
{
	std::shared_ptr<const PoseSequence> retired;
	std::lock_guard lock(m_mutex);
	if (m_sequence)
		retired = m_sequence->Program();
	const auto owner = m_liveInput.Status().owner;
	return owner && m_liveInput.Expired(now) ? ReleaseSourceCommandsLocked(*owner, false)
											 : std::array<bool, ControlChannelCount>{};
}

std::array<bool, ControlChannelCount> CommandScheduler::ReleaseSourceCommandsLocked(CommandSource source, bool retire)
{
	try
	{
		auto pending = m_pending;
		auto outcomes = m_outcomes;
		std::vector<CommandOutcome> cancelled;
		for (auto it = pending.begin(); it != pending.end();)
			if (it->transaction.source == source)
			{
				it->result = CommandResult::Cancelled;
				if (!m_applied.empty())
				{
					it->appliedFrame = m_applied.back()->frame;
					it->appliedTime = m_applied.back()->time;
				}
				cancelled.push_back(*it);
				outcomes.push_back(*it);
				if (outcomes.size() > m_historyCapacity)
					outcomes.pop_front();
				it = pending.erase(it);
			}
			else
				++it;
		const bool recorded = m_recorder.Observe(cancelled);
		const auto retained = std::min(cancelled.size(), m_historyCapacity);
		for (auto index = outcomes.size() - retained; index < outcomes.size(); ++index)
			outcomes[index].recordingOverrun = !recorded;
		m_pending.swap(pending);
		m_outcomes.swap(outcomes);
	}
	catch (const std::bad_alloc&)
	{
		for (auto& command : m_pending)
			if (command.transaction.source == source)
			{
				command.result = CommandResult::Cancelled;
				command.recordingOverrun = true;
			}
		m_recorder.Fail();
	}
	auto released = ReleaseSourceLocked(source);
	const auto sequenceReleased = StopPoseSequenceLocked(source);
	for (std::size_t index = 0; index < released.size(); ++index)
		released[index] = released[index] || sequenceReleased[index];
	if (m_recordingOwner == source)
	{
		m_recorder.Stop();
		m_recordingOwner.reset();
	}
	if (retire)
		std::erase_if(m_sources, [&](const auto& value) { return value.source == source; });
	return released;
}

std::optional<PoseSequenceStatus> CommandScheduler::StartPoseSequence(CommandSource owner,
																	  std::shared_ptr<const PoseSequence> program)
{
	if (!ValidSource(owner) || !program)
		throw std::invalid_argument("Invalid sequence source or program");
	std::optional<PoseSequenceExecution> retired;
	std::lock_guard lock(m_mutex);
	if (m_closed)
		return {};
	if (m_sequence && m_sequence->CanPrepare() && m_sequence->Status().owner != owner)
		return {};
	if (m_nextSequence == std::numeric_limits<std::uint64_t>::max())
		throw std::overflow_error("Sequence identities exhausted");
	const auto previous = m_applied.empty() ? 0 : m_applied.back()->frame.value;
	if (previous == std::numeric_limits<std::uint64_t>::max() ||
		(program->Size() &&
		 program->Steps().back().frame.value > std::numeric_limits<std::uint64_t>::max() - previous - 1))
		throw std::overflow_error("Pose sequence extends beyond the frame counter");
	const auto previousSource = m_replacedSequence.value_or(m_sequence ? m_sequence->Source() : CommandSource{});
	PoseSequenceExecution prepared(m_nextSequence, owner, std::move(program), m_state.OwnedChannels(previousSource));
	if (m_sequence && m_sequence->Status().origin.value)
		m_replacedSequence = m_sequence->Source();
	retired.swap(m_sequence);
	m_sequence.emplace(std::move(prepared));
	++m_nextSequence;
	return m_sequence->Status();
}

std::optional<PoseSequenceStatus> CommandScheduler::PoseSequenceStatusFor(CommandSource owner) const
{
	std::lock_guard lock(m_mutex);
	return m_sequence && m_sequence->Status().owner == owner ? std::optional{m_sequence->Status()} : std::nullopt;
}

std::array<bool, ControlChannelCount> CommandScheduler::ReleaseSourceLocked(CommandSource source) noexcept
{
	const auto channels = m_state.ReleaseSource(source);
	if (m_liveInput.Status().owner == source)
		m_liveInput.Reset();
	try
	{
		const auto latest = m_applied.empty() ? nullptr : m_applied.back();
		if (auto outcome = ReleaseOutcome(source, channels, latest ? latest->frame : FrameId{},
										  latest ? latest->time : SimulationTime{}, true))
		{
			if (m_baseline)
			{
				const auto runtimeTime = latest ? latest->runtimeTime : RuntimeTime{Duration{1}};
				outcome->appliedRuntimeTime = runtimeTime;
				outcome->releaseState =
					ReleaseState(m_state, *m_baseline, outcome->appliedFrame, outcome->appliedTime, runtimeTime);
			}
			m_recorder.Observe(*outcome);
		}
	}
	catch (...)
	{
		m_recorder.Fail();
	}
	return channels;
}

std::array<bool, ControlChannelCount> CommandScheduler::StopPoseSequenceLocked(CommandSource owner) noexcept
{
	std::array<bool, ControlChannelCount> released{};
	if (!m_sequence || m_sequence->Status().owner != owner)
		return released;
	released = ReleaseSourceLocked(m_sequence->Source());
	if (m_replacedSequence)
	{
		const auto previous = ReleaseSourceLocked(*m_replacedSequence);
		for (std::size_t index = 0; index < released.size(); ++index)
			released[index] = released[index] || previous[index];
		m_replacedSequence.reset();
	}
	m_sequence->Cancel();
	return released;
}

std::array<bool, ControlChannelCount> CommandScheduler::StopPoseSequence(CommandSource owner)
{
	std::shared_ptr<const PoseSequence> retired;
	std::lock_guard lock(m_mutex);
	if (m_sequence)
		retired = m_sequence->Program();
	return StopPoseSequenceLocked(owner);
}

RecordingResult CommandScheduler::StartRecording(const CommandConfiguration& baseline,
												 const ReferenceSpaceState& spaces, bool drain, CommandSource owner)
{
	std::lock_guard lock(m_mutex);
	if (!ValidSource(owner))
		return RecordingResult::Invalid;
	if (m_closed)
		return RecordingResult::Closed;
	if (m_recordingOwner && *m_recordingOwner != owner)
		return RecordingResult::Busy;
	if (m_recorder.Snapshot({}, 0).active)
		return RecordingResult::Conflict;
	const auto latest = m_applied.empty() ? nullptr : m_applied.back();
	auto origin = m_state.RecordingOrigin(baseline, latest ? latest->runtimeTime : RuntimeTime{Duration{1}});
	origin.seed = m_seed;
	origin.configuration.referenceSpaces = spaces;
	if (latest)
	{
		origin.frame = latest->frame;
		origin.time = latest->time;
		origin.runtimeTime = latest->runtimeTime;
	}
	std::optional<CommandConfiguration> retainedBaseline{baseline};
	m_recorder.Start(origin, drain);
	m_recordingOwner = owner;
	m_baseline.swap(retainedBaseline);
	return RecordingResult::Ok;
}

RecordingResult CommandScheduler::StopRecording(CommandSource owner, std::uint64_t identity)
{
	std::lock_guard lock(m_mutex);
	if (!ValidSource(owner))
		return RecordingResult::Invalid;
	if (m_recordingOwner && *m_recordingOwner != owner)
		return RecordingResult::Busy;
	const auto state = m_recorder.Snapshot({}, 0);
	if ((state.drain || identity) && state.identity != identity)
		return RecordingResult::Conflict;
	m_recorder.Stop();
	if (!state.drain)
		m_recordingOwner.reset();
	return RecordingResult::Ok;
}

RecordingResult CommandScheduler::AcknowledgeRecording(CommandSource owner, std::uint64_t identity, std::size_t through)
{
	std::lock_guard lock(m_mutex);
	if (!ValidSource(owner) || !identity)
		return RecordingResult::Invalid;
	if (m_recordingOwner != owner)
		return RecordingResult::Busy;
	const auto state = m_recorder.Snapshot({}, 0);
	if (!state.drain || state.identity != identity)
		return RecordingResult::Conflict;
	if (through > state.total)
		return RecordingResult::Invalid;
	m_recorder.Acknowledge(through);
	return RecordingResult::Ok;
}

CommandConfiguration CommandScheduler::Merge(const CommandConfiguration& baseline) const
{
	std::lock_guard lock(m_mutex);
	return m_state.Merge(baseline);
}

std::shared_ptr<const AppliedCommandSnapshot> CommandScheduler::At(FrameId frame) const
{
	std::lock_guard lock(m_mutex);
	for (const auto& value : m_applied)
		if (value->frame == frame)
			return value;
	return {};
}

CommandRecorder& CommandScheduler::Recorder()
{
	return m_recorder;
}

std::shared_ptr<const AppliedCommandSnapshot>
CommandScheduler::Apply(FrameId frame, SimulationTime time, RuntimeTime runtimeTime,
						const CommandConfiguration& baseline,
						const std::function<void(const std::shared_ptr<const AppliedCommandSnapshot>&)>& reserve,
						LiveInputClock::time_point now, std::optional<std::uint64_t> clockRevision)
{
	// Retire replaced state after releasing admission, including reservation failure.
	std::optional<PreparedCommandBoundary> prepared;
	std::lock_guard lock(m_mutex);
	if (m_closed || !frame.value || runtimeTime.value.count() <= 0 || time.value.count() < 0)
		throw std::invalid_argument("Invalid command boundary");
	if (!m_applied.empty() && frame <= m_applied.back()->frame)
	{
		for (const auto& snapshot : m_applied)
			if (snapshot->frame == frame)
				return snapshot;
		throw std::invalid_argument("Command boundary moved backward");
	}
	if (!m_applied.empty() && time < m_applied.back()->time)
		throw std::invalid_argument("Simulation time moved backward");
	prepared.emplace(*this, frame, time, runtimeTime, baseline, now, clockRevision);
	const auto reserveAndCommit = [&](bool recorded) {
		// The external reservation is the last throwing operation. Both the
		// recorder and scheduler retain their previous state if it fails.
		prepared->MarkRecordingOverrun(!recorded);
		if (reserve)
			reserve(prepared->Snapshot());
		prepared->Commit(*this);
	};
	m_recorder.ObservePrepared(prepared->Outcomes(), reserveAndCommit);
	return prepared->Snapshot();
}
} // namespace simxr
