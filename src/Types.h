#pragma once

#include <chrono>
#include <compare>

namespace simxr
{
using Duration = std::chrono::nanoseconds;

struct RuntimeTime
{
	Duration value{};
	auto operator<=>(const RuntimeTime&) const = default;
};
}
