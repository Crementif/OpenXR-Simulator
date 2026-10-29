#pragma once

#include <openxr/openxr.h>

#include <chrono>
#include <cstdio>
#include <stdexcept>
#include <string>
#include <thread>

namespace simxr::probe
{
template<typename Visitor>
void PollXrEvents(XrInstance instance, Visitor&& visit)
{
	while (true)
	{
		XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
		const auto result = xrPollEvent(instance, &event);
		if (result == XR_EVENT_UNAVAILABLE)
			return;
		if (result != XR_SUCCESS)
			throw std::runtime_error("xrPollEvent failed: " + std::to_string(result));
		visit(event);
	}
}

inline XrSessionState BeginProbeSession(XrInstance instance, XrSession session, XrViewConfigurationType viewType)
{
	XrSessionState state = XR_SESSION_STATE_UNKNOWN;
	bool began = false;
	for (int attempt = 0; attempt < 200 && !began; ++attempt)
	{
		PollXrEvents(instance, [&](const XrEventDataBuffer& event) {
			if (event.type != XR_TYPE_EVENT_DATA_SESSION_STATE_CHANGED)
				return;
			const auto& changed = reinterpret_cast<const XrEventDataSessionStateChanged&>(event);
			if (changed.session != session)
				throw std::runtime_error("Session state event belongs to another session");
			state = changed.state;
			std::printf("       session state -> %d\n", static_cast<int>(state));
			if (state == XR_SESSION_STATE_READY && !began)
			{
				XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
				begin.primaryViewConfigurationType = viewType;
				const auto result = xrBeginSession(session, &begin);
				if (XR_FAILED(result))
					throw std::runtime_error("xrBeginSession failed: " + std::to_string(result));
				began = true;
			}
		});
		if (!began)
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
	if (!began)
		throw std::runtime_error("Runtime never delivered SESSION_STATE_READY");
	return state;
}
} // namespace simxr::probe
