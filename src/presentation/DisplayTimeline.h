#pragma once

#include "presentation/FrameTypes.h"

namespace simxr
{
class DisplayTimeline
{
public:
	DisplayTimeline(RuntimeTime epoch, Duration period);
	RuntimeTime TimeAt(DisplayTick tick) const;
	DisplayTick TickAtOrAfter(RuntimeTime time) const;
	Duration Period() const;
	RuntimeTime Epoch() const;

private:
	RuntimeTime m_epoch;
	Duration m_period;
};
}
