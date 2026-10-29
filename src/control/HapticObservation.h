#pragma once

#include "control/WireProtocol.h"
#include "simulation/HapticState.h"

namespace simxr::control
{
inline constexpr std::size_t MaxObservedHaptics = 256;

struct ObservedHaptic
{
	std::uint64_t sequence = 0;
	InputDeviceId device;
	float amplitude = 0, frequency = 0;
	std::int64_t start = 0;
	std::optional<std::int64_t> end;
	HapticStatus status = HapticStatus::Active;
};

struct HapticObservation
{
	std::int64_t time = 0;
	bool overflow = false;
	std::vector<ObservedHaptic> events;
};

// Recent output only; action handles never cross the wire or become replay input.
Field EncodeHaptics(std::span<const HapticOutput> outputs);
HapticObservation DecodeHaptics(const Message& message);
} // namespace simxr::control
