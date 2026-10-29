#pragma once

#include <cstdint>
#include <deque>
#include <openxr/openxr.h>

namespace simxr::headset
{
struct HeadsetSpaceSample
{
	XrPosef initialFromCurrent{{0, 0, 0, 1}, {0, 0, 0}};
	std::uint64_t revision = 1;
};

// The host samples monotonically increasing predicted times. Each returned value
// travels with its tracking batch, independent of later runtime origin changes.
class HeadsetSpace
{
public:
	void Schedule(XrTime changeTime, bool valid, XrPosef previousFromNew);
	HeadsetSpaceSample At(XrTime time);

private:
	struct Change
	{
		XrTime time;
		bool valid;
		XrPosef previousFromNew;
	};

	std::deque<Change> m_pending;
	HeadsetSpaceSample m_current;
	XrTime m_lastTime = 0;
	bool m_failed = false;
};
} // namespace simxr::headset
