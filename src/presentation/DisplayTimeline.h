#pragma once

#include "presentation/FrameTypes.h"

#include <memory>
#include <vector>

namespace simxr
{
// Display ticks on a piecewise-regular grid. Each refresh-rate change starts a
// segment at a tick, so earlier ticks keep their times.
class DisplayTimeline
{
public:
	DisplayTimeline(RuntimeTime epoch, Duration period);
	RuntimeTime TimeAt(DisplayTick tick) const;
	DisplayTick TickAtOrAfter(RuntimeTime time) const;
	// The last tick whose time is not after time; the first tick for earlier times.
	DisplayTick TickAtOrBefore(RuntimeTime time) const;
	// The period from the latest change on.
	Duration Period() const;
	RuntimeTime Epoch() const;
	// The same ticks up to boundary, then period. The boundary must not precede the latest change.
	DisplayTimeline Retimed(DisplayTick boundary, Duration period) const;

private:
	struct Segment
	{
		DisplayTick tick;
		RuntimeTime time;
		Duration period;
	};

	const Segment& SegmentFor(DisplayTick tick) const;

	RuntimeTime m_epoch;
	Segment m_current;
	// Shared so copying a timeline stays cheap; empty until the rate first changes.
	std::shared_ptr<const std::vector<Segment>> m_earlier;
};
} // namespace simxr
