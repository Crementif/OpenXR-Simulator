#pragma once

#include "Types.h"

#include <cstddef>
#include <cstdint>
#include <mutex>
#include <openxr/openxr.h>
#include <variant>
#include <vector>

namespace simxr
{
struct SessionStateEvent
{
	XrSession session;
	XrSessionState state;
	RuntimeTime time;
};

struct InteractionProfileEvent
{
	XrSession session;
};

struct ReferenceSpaceEvent
{
	XrSession session;
	XrReferenceSpaceType referenceSpace;
	RuntimeTime changeTime;
	bool poseValid;
	XrPosef poseInPreviousSpace;
};

struct TrackerConnectedEvent
{
	XrPath persistentPath;
	XrPath rolePath;
};

struct InstanceLossEvent
{
	RuntimeTime lossTime;
};

using RuntimeEvent = std::variant<SessionStateEvent, InteractionProfileEvent, ReferenceSpaceEvent, InstanceLossEvent,
								  TrackerConnectedEvent>;

struct ObservedEvent
{
	std::uint64_t sequence;
	RuntimeEvent event;
};

class EventQueue
{
public:
	explicit EventQueue(std::size_t capacity = 256);
	void Push(RuntimeEvent event);
	XrResult Poll(XrEventDataBuffer& buffer);
	void RemoveSession(XrSession session);
	std::vector<ObservedEvent> Observe(std::uint64_t afterSequence = 0) const;

private:
	mutable std::mutex m_mutex;
	const std::size_t m_capacity;
	std::vector<RuntimeEvent> m_pending;
	std::vector<ObservedEvent> m_history;
	std::uint64_t m_lastSequence = 0;
	std::uint32_t m_lostEvents = 0;
};
} // namespace simxr
