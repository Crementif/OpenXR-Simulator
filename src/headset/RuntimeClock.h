#pragma once

#include <cstdint>
#include <openxr/openxr.h>
#include <optional>

namespace simxr::headset
{
class OpenXrRuntime;
struct HeadsetInputSample;

struct HeadsetNativeTimes
{
	std::int64_t predicted = 0;
	std::optional<std::int64_t> bodyReturned;
};

HeadsetNativeTimes RuntimeInputTimes(const OpenXrRuntime& runtime, const HeadsetInputSample& input);
// Converts through the vendor extension into the simulator native OS clock domain.
// Frame-timeline mapping after pause/resume is a separate runtime contract.
std::int64_t RuntimeTimeToHostNanoseconds(const OpenXrRuntime& runtime, XrTime time);
// Verifies the vendor conversion using an actual OS clock sample in both directions.
std::int64_t VerifyRuntimeClock(const OpenXrRuntime& runtime);
} // namespace simxr::headset
