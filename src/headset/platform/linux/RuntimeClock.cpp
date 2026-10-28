#include "headset/RuntimeClock.h"
#include "Pch.h"
#include "headset/OpenXrRuntime.h"
#include "platform/ClockConversion.h"

#include <ctime>
#define XR_USE_TIMESPEC
#include <openxr/openxr_platform.h>

namespace simxr::headset
{
std::int64_t RuntimeTimeToHostNanoseconds(const OpenXrRuntime& runtime, XrTime time)
{
	if (time <= 0)
		throw std::invalid_argument("Physical runtime time must be positive");
	const auto convert =
		reinterpret_cast<PFN_xrConvertTimeToTimespecTimeKHR>(runtime.Procedure("xrConvertTimeToTimespecTimeKHR"));
	timespec value{};
	CheckXr(convert(runtime.Instance(), time, &value), "Convert physical XR time to monotonic clock");
	std::int64_t result = 0;
	if (!std::in_range<std::int64_t>(value.tv_sec) || !std::in_range<std::int64_t>(value.tv_nsec) ||
		!MonotonicPartsToNanoseconds(static_cast<std::int64_t>(value.tv_sec), static_cast<std::int64_t>(value.tv_nsec),
									 result))
		throw std::overflow_error("Physical runtime returned an invalid monotonic clock value");
	return result;
}

std::int64_t VerifyRuntimeClock(const OpenXrRuntime& runtime)
{
	const auto toRuntime =
		reinterpret_cast<PFN_xrConvertTimespecTimeToTimeKHR>(runtime.Procedure("xrConvertTimespecTimeToTimeKHR"));
	timespec original{};
	if (clock_gettime(CLOCK_MONOTONIC, &original) != 0)
		throw std::runtime_error("Cannot sample the host monotonic clock");
	XrTime xrTime = 0;
	CheckXr(toRuntime(runtime.Instance(), &original, &xrTime), "Convert host clock to physical XR time");
	std::int64_t originalNs = 0;
	if (!std::in_range<std::int64_t>(original.tv_sec) ||
		!MonotonicPartsToNanoseconds(static_cast<std::int64_t>(original.tv_sec), original.tv_nsec, originalNs))
		throw std::overflow_error("Host monotonic clock is unrepresentable");
	const auto converted = RuntimeTimeToHostNanoseconds(runtime, xrTime);
	const auto error = converted - originalNs;
	if (error < -1000 || error > 1000)
		throw std::runtime_error("Physical runtime clock conversion differs by more than one microsecond");
	return static_cast<std::int64_t>(error);
}
} // namespace simxr::headset
