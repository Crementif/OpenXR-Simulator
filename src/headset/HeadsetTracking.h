#pragma once

#include "headset/HeadsetActions.h"
#include "presentation/FrameScheduler.h"
#include "simulation/BodyObservation.h"
#include "simulation/LiveInputSlot.h"

namespace simxr::headset
{
// Times are already converted through the native clock bridge. Rest skeletons
// retain provider coordinates; only located joint poses receive world calibration.
BodyObservation MapHeadsetBody(const PhysicalBodySample& input, RuntimeTime requested,
							   std::optional<RuntimeTime> returned, XrPosef worldFromBase);

struct HeadsetAlignment
{
	XrPosef worldHead{{0, 0, 0, 1}, {0, 0, 0}};
	float worldFloor = 0;
	std::optional<float> localEyeHeight;
};
XrPosef AlignHeadset(const HeadsetInputSample& input, const HeadsetAlignment& alignment);
HeadsetAlignment HeadsetRestartAlignment(const HeadsetInputSample& input, XrPosef worldFromInitial, float worldFloor);

// Native timestamps must come from the domain advertised by the simulator.
// The explicit transform maps the initial physical base space into simulator world.
// Each sample carries the runtime origin adjustment effective at its predicted time.
LiveInputSample MapHeadsetTracking(const HeadsetInputSample& input, const FrameNativeClockMapping& clock,
								   NativeClockDomain domain, std::int64_t observed, std::int64_t predicted,
								   XrPosef worldFromPhysical, std::uint64_t attachment, std::uint64_t sequence,
								   Duration maximumUncertainty = std::chrono::milliseconds(1),
								   std::uint64_t spaceRevision = 1, std::optional<std::int64_t> bodyReturned = {});
} // namespace simxr::headset
