#include "platform/ClockConversion.h"
#include "Pch.h"

namespace simxr
{
bool ScalePositiveTime(std::int64_t value, std::int64_t numerator, std::int64_t denominator,
					   std::int64_t& output) noexcept
{
	if (value <= 0 || numerator <= 0 || denominator <= 0)
		return false;
	const auto whole = static_cast<std::uint64_t>(value / denominator);
	const auto remainder = static_cast<std::uint64_t>(value % denominator);
	const auto scale = static_cast<std::uint64_t>(numerator);
	const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	if (whole > maximum / scale || remainder > std::numeric_limits<std::uint64_t>::max() / scale)
		return false;
	const auto fractional = remainder * scale / static_cast<std::uint64_t>(denominator);
	const auto integral = whole * scale;
	if (fractional > maximum - integral)
		return false;
	output = static_cast<std::int64_t>(integral + fractional);
	return true;
}

bool MonotonicPartsToNanoseconds(std::int64_t seconds, std::int64_t nanoseconds, std::int64_t& output) noexcept
{
	if (seconds < 0 || nanoseconds < 0 || nanoseconds >= 1000000000 ||
		seconds > (std::numeric_limits<std::int64_t>::max() - nanoseconds) / 1000000000)
		return false;
	const auto result = seconds * 1000000000 + nanoseconds;
	if (result <= 0)
		return false;
	output = result;
	return true;
}
} // namespace simxr
