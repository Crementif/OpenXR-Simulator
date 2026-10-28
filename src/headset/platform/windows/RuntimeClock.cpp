#include "headset/RuntimeClock.h"
#include "Pch.h"
#include "headset/OpenXrRuntime.h"
#include "platform/ClockConversion.h"

#include <unknwn.h>
#include <windows.h>
#define XR_USE_PLATFORM_WIN32
#include <openxr/openxr_platform.h>

namespace simxr::headset
{
std::int64_t RuntimeTimeToHostNanoseconds(const OpenXrRuntime& runtime, XrTime time)
{
	if (time <= 0)
		throw std::invalid_argument("Physical runtime time must be positive");
	const auto convert = reinterpret_cast<PFN_xrConvertTimeToWin32PerformanceCounterKHR>(
		runtime.Procedure("xrConvertTimeToWin32PerformanceCounterKHR"));
	LARGE_INTEGER counter{}, frequency{};
	CheckXr(convert(runtime.Instance(), time, &counter), "Convert physical XR time to performance counter");
	if (!QueryPerformanceFrequency(&frequency))
		throw std::runtime_error("Cannot read performance counter frequency");
	std::int64_t result = 0;
	if (!ScalePositiveTime(counter.QuadPart, 1000000000, frequency.QuadPart, result))
		throw std::overflow_error("Physical runtime returned an invalid performance counter");
	return result;
}

std::int64_t VerifyRuntimeClock(const OpenXrRuntime& runtime)
{
	const auto toRuntime = reinterpret_cast<PFN_xrConvertWin32PerformanceCounterToTimeKHR>(
		runtime.Procedure("xrConvertWin32PerformanceCounterToTimeKHR"));
	LARGE_INTEGER original{}, frequency{};
	if (!QueryPerformanceCounter(&original) || !QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
		throw std::runtime_error("Cannot sample the host performance counter");
	XrTime xrTime = 0;
	CheckXr(toRuntime(runtime.Instance(), &original, &xrTime), "Convert host clock to physical XR time");
	std::int64_t originalNs = 0;
	if (!ScalePositiveTime(original.QuadPart, 1000000000, frequency.QuadPart, originalNs))
		throw std::overflow_error("Host performance counter is unrepresentable");
	const auto error = RuntimeTimeToHostNanoseconds(runtime, xrTime) - originalNs;
	if (error < -1000 || error > 1000)
		throw std::runtime_error("Physical runtime clock conversion differs by more than one microsecond");
	return static_cast<std::int64_t>(error);
}
} // namespace simxr::headset
