#include "runtime/EventQueue.h"
#include "../TestSupport.h"

#include <iostream>

int main()
{
	using namespace simxr;
	auto session = reinterpret_cast<XrSession>(1);
	EventQueue events(2);
	XrEventDataBuffer buffer{XR_TYPE_EVENT_DATA_BUFFER};
	Require(events.Poll(buffer) == XR_EVENT_UNAVAILABLE, "Empty polling");
	events.Push(SessionStateEvent{session, XR_SESSION_STATE_READY, {Duration{5}}});
	events.Push(InteractionProfileEvent{session});
	auto observations = events.Observe();
	Require(observations.size() == 2, "Observation history");
	Require(events.Observe(observations.front().sequence).size() == 1, "Observation cursor");
	Require(events.Poll(buffer) == XR_SUCCESS, "First poll");
	auto state = reinterpret_cast<const XrEventDataSessionStateChanged*>(&buffer);
	Require(state->type == XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED && state->time == 5 && state->next == nullptr, "Owned event serialization");
	buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
	Require(events.Poll(buffer) == XR_SUCCESS && buffer.type == XR_TYPE_EVENT_DATA_INTERACTION_PROFILE_CHANGED, "FIFO order");
	Require(events.Observe().size() == 2, "Polling consumed observer history");
	events.Push(SessionStateEvent{session, XR_SESSION_STATE_VISIBLE, {Duration{7}}});
	events.Push(SessionStateEvent{session, XR_SESSION_STATE_FOCUSED, {Duration{8}}});
	events.Push(InstanceLossEvent{{Duration{9}}});
	buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
	Require(events.Poll(buffer) == XR_SUCCESS && buffer.type == XR_TYPE_EVENT_DATA_EVENTS_LOST, "Missing overflow event");
	Require(reinterpret_cast<const XrEventDataEventsLost*>(&buffer)->lostEventCount == 1, "Wrong overflow count");
	events.RemoveSession(session);
	buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
	Require(events.Poll(buffer) == XR_SUCCESS && buffer.type == XR_TYPE_EVENT_DATA_INSTANCE_LOSS_PENDING, "Destroyed-session event escaped");
	buffer.type = XR_TYPE_EVENT_DATA_BUFFER;
	Require(events.Poll(buffer) == XR_EVENT_UNAVAILABLE, "Stale event remained");
	Require(events.Observe().size() == 2, "Unbounded observer history");
	std::cout << "Event queue tests passed\n";
}
