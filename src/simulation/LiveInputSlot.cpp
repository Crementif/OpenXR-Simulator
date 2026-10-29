#include "simulation/LiveInputSlot.h"
#include "Pch.h"
#include "math/Math.h"
#include "simulation/BodyObservation.h"
#include "simulation/TrackingSystem.h"
#include "simulation/ViewConfiguration.h"

namespace simxr
{
static std::uint32_t DigitalState(const ControllerControls& controls)
{
	constexpr std::array members{
		&ControllerControls::primary,	   &ControllerControls::secondary,		&ControllerControls::menu,
		&ControllerControls::back,		   &ControllerControls::thumbClick,		&ControllerControls::trackpadClick,
		&ControllerControls::primaryTouch, &ControllerControls::secondaryTouch, &ControllerControls::triggerTouch,
		&ControllerControls::thumbTouch,   &ControllerControls::trackpadTouch,	&ControllerControls::thumbrestTouch,
		&ControllerControls::connected};
	std::uint32_t state = 0;
	for (std::size_t index = 0; index < members.size(); ++index)
		if (controls.*members[index])
			state |= 1U << index;
	// The action resolver also exposes thresholded analog values as booleans.
	for (const auto value : {controls.trigger, controls.grip, controls.gripForce, controls.trackpadForce})
		state = (state << 1) | (value > 0.5f);
	state = (state << 1) | controls.triggerClick.value_or(controls.trigger > .5f);
	return state;
}

static bool NormalizeLivePose(PoseSample& sample)
{
	const auto normalized = NormalizedPose(sample.pose);
	if (!normalized || (sample.flags & ~TrackedPoseFlags) ||
		((sample.flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) &&
		 !(sample.flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) ||
		((sample.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) &&
		 !(sample.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)))
		return false;
	sample.pose = *normalized;
	return true;
}

static bool SameTrackerTopology(const std::vector<TrackerDevice>& left, const std::vector<TrackerDevice>& right)
{
	if (left.size() != right.size())
		return false;
	for (std::size_t index = 0; index < left.size(); ++index)
		if (left[index].identity != right[index].identity || left[index].sourcePath != right[index].sourcePath ||
			left[index].role != right[index].role || left[index].connected != right[index].connected)
			return false;
	return true;
}

bool LiveInputSlot::HasChannels(const Channels& owned)
{
	return owned[static_cast<std::size_t>(ControlChannel::Body)] ||
		   owned[static_cast<std::size_t>(ControlChannel::Trackers)] ||
		   owned[static_cast<std::size_t>(ControlChannel::Views)] ||
		   std::any_of(owned.begin(), owned.begin() + 5, [](bool value) { return value; });
}

void LiveInputSlot::Reset()
{
	*this = {};
}

void LiveInputSlot::AcknowledgeControls()
{
	for (auto& queue : m_transitions)
		queue.awaitingSync = false;
	if (m_trackersAwaitingSync)
		m_dirty = true;
	m_trackersAwaitingSync = false;
}

LiveInputStatus LiveInputSlot::Status() const
{
	return m_status;
}

bool LiveInputSlot::Expired(LiveInputClock::time_point now) const
{
	return m_status.owner && (m_status.phase != LiveInputPhase::Active || now - m_received >= m_latest.maxAge);
}

CommandResult LiveInputSlot::Write(CommandSource source, const LiveInputSample& sample, const Channels& owned,
								   LiveInputClock::time_point now)
{
	if (source.kind != CommandSourceKind::Device || !source.id || !sample.connection || !sample.sequence ||
		sample.observedTime.value.count() <= 0 || sample.targetTime < sample.observedTime ||
		sample.maxAge < std::chrono::milliseconds(1) || sample.maxAge > std::chrono::milliseconds(250))
		return CommandResult::Invalid;
	if (!HasChannels(owned) ||
		(m_status.owner && (*m_status.owner != source || m_status.connection != sample.connection ||
							m_status.clockRevision != sample.clockRevision)))
		return CommandResult::Conflict;
	if (m_status.owner && (m_status.phase != LiveInputPhase::Active || Expired(now)))
		return CommandResult::Cancelled;
	if (sample.sequence <= m_status.sequence)
		return CommandResult::Applied;
	if (m_status.owner && (sample.observedTime < m_latest.observedTime || now < m_received))
		return CommandResult::Invalid;
	if (sample.trackers.has_value() != owned[static_cast<std::size_t>(ControlChannel::Trackers)])
		return CommandResult::Conflict;
	if (sample.body.has_value() != owned[static_cast<std::size_t>(ControlChannel::Body)])
		return CommandResult::Conflict;
	auto normalized = sample;
	if (normalized.body)
	{
		if (normalized.body->requestedTime != normalized.targetTime)
			return CommandResult::Invalid;
		try
		{
			normalized.body = NormalizeBodyObservation(*normalized.body);
		}
		catch (const std::invalid_argument&)
		{
			return CommandResult::Invalid;
		}
	}
	if (normalized.trackers)
	{
		if (normalized.trackers->size() > MaxTrackerDevices)
			return CommandResult::Invalid;
		for (const auto& tracker : *normalized.trackers)
			if (!tracker.poseOverride || tracker.anchor || tracker.jointOffset)
				return CommandResult::Invalid;
		try
		{
			TrackingState state;
			state.trackers = *normalized.trackers;
			normalized.trackers = TrackingSystem::Evaluate(sample.targetTime, state, BodyModel{}).input.trackers;
		}
		catch (const std::invalid_argument&)
		{
			return CommandResult::Invalid;
		}
	}
	if (!NormalizeLivePose(normalized.head))
		return CommandResult::Invalid;
	if (normalized.eyeViews)
	{
		if (!normalized.eyeViews->spaceRevision)
			return CommandResult::Invalid;
		for (std::size_t eye = 0; eye < 2; ++eye)
			if (!NormalizeLivePose(normalized.eyeViews->poses[eye]) ||
				!ValidViewFieldOfView(normalized.eyeViews->fovs[eye]))
				return CommandResult::Invalid;
	}
	InputSnapshot controls;
	controls.controllers = sample.controls;
	if (XR_FAILED(InputState::Validate(controls)))
		return CommandResult::Invalid;
	for (auto& hand : normalized.hands)
		if (!NormalizeLivePose(hand.pose) || (hand.aim && !NormalizeLivePose(*hand.aim)))
			return CommandResult::Invalid;
	const bool suspend = !normalized.controlsActive && !m_controlsSuspended;
	if (!normalized.controlsActive)
	{
		for (auto& value : normalized.controls)
		{
			auto profile = std::move(value.physicalProfile);
			const bool connected = value.connected;
			value = {};
			value.physicalProfile = std::move(profile);
			value.connected = connected;
		}
	}
	std::optional<TrackerTransition> trackerTransition;
	if (normalized.trackers && (!m_latest.trackers || !SameTrackerTopology(*normalized.trackers, *m_latest.trackers)))
	{
		if (m_trackerCount == m_trackerTransitions.size())
		{
			m_status.phase = LiveInputPhase::Overflow;
			return CommandResult::Capacity;
		}
		trackerTransition = TrackerTransition{*normalized.trackers, normalized.observedTime};
	}
	if (trackerTransition)
	{
		m_trackerTransitions[(m_trackerFirst + m_trackerCount) % m_trackerTransitions.size()] =
			std::move(*trackerTransition);
		++m_trackerCount;
	}
	if (suspend)
		m_transitions = {};
	m_controlsSuspended = !normalized.controlsActive;
	m_status.owner = source;
	m_status.connection = sample.connection;
	m_status.sequence = sample.sequence;
	m_status.clockRevision = sample.clockRevision;
	m_status.phase = LiveInputPhase::Active;
	if (m_dirty)
		++m_status.coalesced;
	static_assert(std::is_nothrow_move_assignable_v<LiveInputSample>);
	m_latest = std::move(normalized);
	m_received = now;
	m_dirty = true;
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		auto& queue = m_transitions[hand];
		if (!owned[static_cast<std::size_t>(ControlChannel::LeftControls) + hand])
		{
			queue = {};
			continue;
		}
		if (!suspend && DigitalState(queue.last) == DigitalState(m_latest.controls[hand]) &&
			queue.last.physicalProfile == m_latest.controls[hand].physicalProfile)
			continue;
		if (queue.count == queue.values.size())
		{
			m_status.phase = LiveInputPhase::Overflow;
			m_transitions = {};
			return CommandResult::Capacity;
		}
		queue.values[(queue.first + queue.count++) % queue.values.size()] = {m_latest.controls[hand],
																			 sample.observedTime};
		queue.last = m_latest.controls[hand];
	}
	return CommandResult::Queued;
}

std::optional<CommandOutcome> LiveInputSlot::Prepare(FrameId frame, SimulationTime time, const Channels& owned,
													 LiveInputClock::time_point now,
													 std::optional<std::uint64_t> clockRevision)
{
	if (!m_status.owner)
		return {};
	if (!HasChannels(owned))
	{
		Reset();
		return {};
	}
	if (clockRevision && *clockRevision != m_status.clockRevision)
		m_status.phase = LiveInputPhase::Stale;
	const bool expired = Expired(now);
	if (expired && m_suspendedApplied)
		return {};
	if (!m_dirty && !m_transitions[0].count && !m_transitions[1].count && !m_trackerCount && !expired)
		return {};
	CommandOutcome outcome;
	outcome.transaction.source = *m_status.owner;
	outcome.transaction.target.frame = frame;
	outcome.appliedFrame = frame;
	outcome.appliedTime = time;
	outcome.liveInput = LiveInputCorrelation{m_latest.connection,
											 m_latest.sequence,
											 m_latest.observedTime,
											 m_latest.targetTime,
											 {m_latest.observedTime, m_latest.observedTime}};
	outcome.liveInput->clockRevision = m_latest.clockRevision;
	if (expired)
	{
		m_suspendedApplied = true;
		if (m_status.phase != LiveInputPhase::Overflow)
			m_status.phase = LiveInputPhase::Stale;
		m_transitions = {};
		m_trackerCount = 0;
		m_trackersAwaitingSync = false;
		if (m_latest.trackers)
			for (auto& tracker : *m_latest.trackers)
			{
				tracker.poseFlags = 0;
				tracker.tracked = false;
			}
		if (m_latest.body)
		{
			for (auto& joint : m_latest.body->joints)
				if (joint)
					joint->flags = 0;
			if (m_latest.body->active)
				m_latest.body->active = false;
			if (m_latest.body->confidence)
				m_latest.body->confidence = 0;
			if (m_latest.body->allJointPosesTracked)
				m_latest.body->allJointPosesTracked = false;
		}
		m_latest.head.flags = 0;
		if (m_latest.eyeViews)
			for (auto& pose : m_latest.eyeViews->poses)
				pose.flags = 0;
		for (std::size_t hand = 0; hand < 2; ++hand)
		{
			m_latest.hands[hand].pose.flags = 0;
			if (m_latest.hands[hand].aim)
				m_latest.hands[hand].aim->flags = 0;
			m_latest.controls[hand] = {};
			m_latest.controls[hand].connected = false;
		}
	}
	if (owned[static_cast<std::size_t>(ControlChannel::Trackers)] && m_latest.trackers)
	{
		const auto* devices = &*m_latest.trackers;
		outcome.liveInput->trackersObservedTime = m_latest.observedTime;
		if (!m_trackersAwaitingSync && m_trackerCount)
		{
			m_publishedTrackers = std::move(m_trackerTransitions[m_trackerFirst]);
			m_trackerFirst = (m_trackerFirst + 1) % m_trackerTransitions.size();
			--m_trackerCount;
			m_trackersAwaitingSync = true;
		}
		if (m_trackersAwaitingSync)
		{
			devices = &m_publishedTrackers.devices;
			outcome.liveInput->trackersObservedTime = m_publishedTrackers.observedTime;
		}
		SetTrackers command{*devices};
		if (m_trackersAwaitingSync)
			for (auto& tracker : command.value)
			{
				const auto current =
					std::find_if(m_latest.trackers->begin(), m_latest.trackers->end(), [&](const TrackerDevice& value) {
						return value.identity == tracker.identity && value.sourcePath == tracker.sourcePath;
					});
				if (current == m_latest.trackers->end() || !current->connected)
				{
					tracker.poseFlags = 0;
					tracker.tracked = false;
				}
				else
				{
					tracker.poseFlags &= current->poseFlags;
					tracker.tracked = tracker.tracked && current->tracked;
				}
			}
		outcome.transaction.commands.push_back(std::move(command));
	}
	else
	{
		m_trackerCount = 0;
		m_trackersAwaitingSync = false;
	}
	if (owned[static_cast<std::size_t>(ControlChannel::Body)] && m_latest.body)
		outcome.transaction.commands.push_back(SetBodyControls{{}, m_latest.body});
	if (owned[static_cast<std::size_t>(ControlChannel::Views)])
		outcome.transaction.commands.push_back(SetEyeViews{m_latest.eyeViews});
	if (owned[static_cast<std::size_t>(ControlChannel::Head)])
		outcome.transaction.commands.push_back(SetHeadPose{m_latest.head});
	for (std::uint32_t hand = 0; hand < 2; ++hand)
	{
		if (owned[static_cast<std::size_t>(ControlChannel::LeftPose) + hand])
			outcome.transaction.commands.push_back(SetControllerPose{hand, m_latest.hands[hand]});
		if (owned[static_cast<std::size_t>(ControlChannel::LeftControls) + hand])
		{
			auto& queue = m_transitions[hand];
			auto controls = m_latest.controls[hand];
			if (queue.awaitingSync)
			{
				controls = queue.published;
				outcome.liveInput->controlsObservedTime[hand] = queue.publishedTime;
			}
			else if (queue.count)
			{
				controls = queue.values[queue.first].controls;
				queue.publishedTime = queue.values[queue.first].observedTime;
				outcome.liveInput->controlsObservedTime[hand] = queue.publishedTime;
				queue.first = (queue.first + 1) % queue.values.size();
				--queue.count;
				queue.published = controls;
				queue.awaitingSync = true;
			}
			outcome.transaction.commands.push_back(SetControllerControls{hand, controls});
		}
		else
			m_transitions[hand] = {};
	}
	m_dirty = false;
	return outcome;
}
} // namespace simxr
