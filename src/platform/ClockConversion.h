#pragma once

#include <cstdint>

namespace simxr
{
// Positive clock values only; failed conversion leaves output unchanged.
bool ScalePositiveTime(std::int64_t value, std::int64_t numerator, std::int64_t denominator,
					   std::int64_t& output) noexcept;
bool MonotonicPartsToNanoseconds(std::int64_t seconds, std::int64_t nanoseconds, std::int64_t& output) noexcept;
} // namespace simxr
