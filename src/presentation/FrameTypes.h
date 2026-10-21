#pragma once

#include "Types.h"

#include <compare>
#include <cstdint>

namespace simxr
{
struct FrameId
{
	std::uint64_t value = 0;
	auto operator<=>(const FrameId&) const = default;
};

struct DisplayTick
{
	std::uint64_t value = 0;
	auto operator<=>(const DisplayTick&) const = default;
};

struct SessionGeneration
{
	std::uint64_t value = 0;
	auto operator<=>(const SessionGeneration&) const = default;
};

struct FrameKey
{
	SessionGeneration generation;
	FrameId frame;
	auto operator<=>(const FrameKey&) const = default;
};

enum class ExecutionMode
{
	RealTime,
	Deterministic,
	Unpaced
};

struct FramePrediction
{
	FrameId id;
	DisplayTick displayTick;
	RuntimeTime displayTime;
	Duration displayPeriod{};
};
} // namespace simxr
