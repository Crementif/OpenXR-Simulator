#include "Pch.h"
#include "platform/Clock.h"
#include "platform/ClockConversion.h"

#include <stdexcept>
#include <utility>
#ifdef _WIN32
#include <windows.h>
#else
#include <ctime>
#endif

namespace simxr
{
NativeClockSample ReadNativeClockSample()
{
	NativeClockSample sample{};
#ifdef _WIN32
	sample.domain = NativeClockDomain::PerformanceCounterNanoseconds;
	LARGE_INTEGER frequency{}, counter{};
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
		throw std::runtime_error("Cannot read performance counter frequency");
	sample.resolutionNanoseconds = 1000000000 / frequency.QuadPart + (1000000000 % frequency.QuadPart != 0);
	sample.before = std::chrono::steady_clock::now();
	const bool read = QueryPerformanceCounter(&counter) != 0;
	sample.after = std::chrono::steady_clock::now();
	if (!read || !ScalePositiveTime(counter.QuadPart, 1000000000, frequency.QuadPart, sample.nanoseconds))
		throw std::runtime_error("Cannot correlate performance counter with scheduler clock");
#else
	sample.domain = NativeClockDomain::MonotonicNanoseconds;
	timespec resolution{}, value{};
	if (clock_getres(CLOCK_MONOTONIC, &resolution) != 0 || !std::in_range<std::int64_t>(resolution.tv_sec) ||
		!MonotonicPartsToNanoseconds(static_cast<std::int64_t>(resolution.tv_sec), resolution.tv_nsec,
									 sample.resolutionNanoseconds))
		throw std::runtime_error("Cannot read monotonic clock resolution");
	sample.before = std::chrono::steady_clock::now();
	const bool read = clock_gettime(CLOCK_MONOTONIC, &value) == 0;
	sample.after = std::chrono::steady_clock::now();
	if (!read || !std::in_range<std::int64_t>(value.tv_sec) ||
		!MonotonicPartsToNanoseconds(static_cast<std::int64_t>(value.tv_sec), value.tv_nsec, sample.nanoseconds))
		throw std::runtime_error("Cannot correlate monotonic clock with scheduler clock");
#endif
	if (sample.nanoseconds <= 0 || sample.before > sample.after)
		throw std::runtime_error("Invalid native clock correlation");
	return sample;
}
} // namespace simxr
