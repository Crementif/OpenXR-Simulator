#pragma once

#include "control/WireProtocol.h"
#include "presentation/FrameScheduler.h"
#include "simulation/LiveInputSlot.h"

namespace simxr::control
{
std::vector<Field> EncodeLiveInput(const LiveInputSample& sample);
LiveInputSample DecodeLiveInput(const Message& request);
std::optional<FrameNativeClockMapping> DecodeLiveClock(const Message& response);
} // namespace simxr::control
