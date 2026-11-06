#include "runtime/EventQueue.h"
#include "Pch.h"

namespace simxr
{
template<class Event>
static void WriteEvent(const Event& event, XrEventDataBuffer& output)
{
	static_assert(std::is_trivially_copyable_v<Event>);
	static_assert(sizeof(Event) <= sizeof(output));
	std::memcpy(&output, &event, sizeof(event));
}

static void EncodeEvent(const SessionStateEvent& value, XrEventDataBuffer& output)
{
	WriteEvent(XrEventDataSessionStateChanged{XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED, nullptr, value.session,
											  value.state, value.time.value.count()},
			   output);
}

static void EncodeEvent(const InteractionProfileEvent& value, XrEventDataBuffer& output)
{
	WriteEvent(
		XrEventDataInteractionProfileChanged{XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED, nullptr, value.session},
		output);
}

static void EncodeEvent(const ReferenceSpaceEvent& value, XrEventDataBuffer& output)
{
	WriteEvent(
		XrEventDataReferenceSpaceChangePending{XR_TYPE_EVENT_DATA_REFERENCE_SPACE_CHANGE_PENDING, nullptr,
											   value.session, value.referenceSpace, value.changeTime.value.count(),
											   static_cast<XrBool32>(value.poseValid), value.poseInPreviousSpace},
		output);
}

static void EncodeEvent(const DisplayRefreshRateEvent& value, XrEventDataBuffer& output)
{
	WriteEvent(XrEventDataDisplayRefreshRateChangedFB{XR_TYPE_EVENT_DATA_DISPLAY_REFRESH_RATE_CHANGED_FB, nullptr,
													  value.from, value.to},
			   output);
}

static void EncodeEvent(const InstanceLossEvent& value, XrEventDataBuffer& output)
{
	WriteEvent(
		XrEventDataInstanceLossPending{XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING, nullptr, value.lossTime.value.count()},
		output);
}

static void EncodeEvent(const TrackerConnectedEvent& value, XrEventDataBuffer& output)
{
	struct TrackerConnectedPayload
	{
		XrEventDataViveTrackerConnectedHTCX event;
		XrViveTrackerPathsHTCX paths;
	};

	static_assert(alignof(TrackerConnectedPayload) <= alignof(XrEventDataBuffer));
	// The pointer must address the caller's final buffer, never this local value.
	auto* outputPaths = reinterpret_cast<XrViveTrackerPathsHTCX*>(reinterpret_cast<std::byte*>(&output) +
																  offsetof(TrackerConnectedPayload, paths));
	const TrackerConnectedPayload payload{
		{XR_TYPE_EVENT_DATA_VIVE_TRACKER_CONNECTED_HTCX, nullptr, outputPaths},
		{XR_TYPE_VIVE_TRACKER_PATHS_HTCX, nullptr, value.persistentPath, value.rolePath}};
	WriteEvent(payload, output);
}

EventQueue::EventQueue(std::size_t capacity) : m_capacity(capacity)
{
	if (capacity == 0)
		throw std::invalid_argument("Event queue needs capacity");
	m_pending.reserve(capacity);
	m_history.reserve(capacity);
}

void EventQueue::Push(RuntimeEvent event)
{
	static_assert(std::is_nothrow_move_assignable_v<RuntimeEvent>);
	static_assert(std::is_nothrow_copy_constructible_v<RuntimeEvent>);
	std::lock_guard lock(m_mutex);
	if (m_history.size() == m_capacity)
		m_history.erase(m_history.begin());
	m_history.push_back({++m_lastSequence, event});
	if (m_pending.size() == m_capacity)
	{
		m_pending.erase(m_pending.begin());
		if (m_lostEvents != std::numeric_limits<std::uint32_t>::max())
			++m_lostEvents;
	}
	m_pending.push_back(std::move(event));
}

XrResult EventQueue::Poll(XrEventDataBuffer& buffer)
{
	std::lock_guard lock(m_mutex);
	if (buffer.type != XR_TYPE_EVENT_DATA_BUFFER)
		return XR_ERROR_VALIDATION_FAILURE;
	if (m_lostEvents)
	{
		WriteEvent(XrEventDataEventsLost{XR_TYPE_EVENT_DATA_EVENTS_LOST, nullptr, m_lostEvents}, buffer);
		m_lostEvents = 0;
		return XR_SUCCESS;
	}
	if (m_pending.empty())
		return XR_EVENT_UNAVAILABLE;
	std::visit([&buffer](const auto& event) { EncodeEvent(event, buffer); }, m_pending.front());
	m_pending.erase(m_pending.begin());
	return XR_SUCCESS;
}

void EventQueue::RemoveSession(XrSession session)
{
	std::lock_guard lock(m_mutex);
	std::erase_if(m_pending, [session](const RuntimeEvent& event) {
		return std::visit(
			[session](const auto& value) {
				if constexpr (requires { value.session; })
					return value.session == session;
				return false;
			},
			event);
	});
}

std::vector<ObservedEvent> EventQueue::Observe(std::uint64_t afterSequence) const
{
	std::lock_guard lock(m_mutex);
	std::vector<ObservedEvent> result;
	result.reserve(m_history.size());
	for (const auto& observation : m_history)
		if (observation.sequence > afterSequence)
			result.push_back(observation);
	return result;
}
} // namespace simxr
