#include "../TestSupport.h"
#include "Pch.h"
#include "platform/Clock.h"
#include "platform/ClockConversion.h"

#include <iostream>

namespace simxr::tests
{
static void ClockConversion()
{
	SteadyClock clock;
	const auto correlation = clock.NativeSample();
	Require(correlation && correlation->nanoseconds > 0 && correlation->resolutionNanoseconds > 0 &&
				correlation->before <= correlation->after,
			"Native clock correlation is bracketed and has explicit resolution");
	constexpr auto maximum = std::numeric_limits<std::int64_t>::max();
	std::int64_t result = -1;
	Require(MonotonicPartsToNanoseconds(123, 456789, result) && result == 123000456789,
			"Monotonic conversion preserves subsecond precision");
	Require(MonotonicPartsToNanoseconds(maximum / 1000000000, maximum % 1000000000, result) && result == maximum,
			"Largest representable monotonic timestamp");
	for (const auto [seconds, nanos] :
		 std::array<std::array<std::int64_t, 2>, 6>{{{-1, 0},
													 {0, -1},
													 {0, 0},
													 {1, 1000000000},
													 {maximum / 1000000000, maximum % 1000000000 + 1},
													 {maximum, 0}}})
	{
		result = 17;
		Require(!MonotonicPartsToNanoseconds(seconds, nanos, result) && result == 17,
				"Invalid or overflowing monotonic conversion preserves destination");
	}
	Require(ScalePositiveTime(1234567890123, 1000000000, 10000000, result) && result == 123456789012300,
			"QPC scaling avoids overflowing the direct multiplication");
	Require(ScalePositiveTime(maximum, 1, 1, result) && result == maximum, "Exact identity scale at integer limit");
	Require(ScalePositiveTime(11, 1000000000, 3, result) && result == 3666666666,
			"Fractional QPC conversion has defined truncation");
	for (const auto [value, numerator, denominator] : std::array<std::array<std::int64_t, 3>, 5>{
			 {{0, 1, 1}, {-1, 1, 1}, {1, 0, 1}, {1, 1, 0}, {maximum, 1000000000, 1}}})
	{
		result = 17;
		Require(!ScalePositiveTime(value, numerator, denominator, result) && result == 17,
				"Invalid or overflowing performance counter preserves destination");
	}
}
} // namespace simxr::tests
int main()
{
	try
	{
		simxr::tests::ClockConversion();
		std::cout << "Clock conversion checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
