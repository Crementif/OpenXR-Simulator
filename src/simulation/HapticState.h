#pragma once

#include "Types.h"
#include "simulation/InputState.h"

#include <array>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <openxr/openxr.h>
#include <span>
#include <vector>

namespace simxr
{
enum class HapticDevice
{
	LeftController,
	RightController
};

constexpr InputDeviceId DeviceIdentity(HapticDevice device)
{
	return {InputDeviceKind::Controller, static_cast<std::uint64_t>(device)};
}

constexpr bool operator==(InputDeviceId identity, HapticDevice device)
{
	return identity.kind == InputDeviceKind::Controller && identity.value == static_cast<std::uint64_t>(device);
}

enum class HapticStatus
{
	Active = 0,
	Expired = 1,
	Stopped = 2,
	Replaced = 3,
	FocusLost = 4
};

struct HapticOutput
{
	std::uint64_t sequence;
	InputDeviceId device;
	XrAction action;
	XrPath subactionPath;
	float amplitude;
	float frequency;
	XrDuration requestedDuration;
	RuntimeTime start;
	std::optional<RuntimeTime> end;
	HapticStatus status = HapticStatus::Active;
};

struct HapticSnapshot
{
	RuntimeTime time;
	std::vector<HapticOutput> outputs;
};

class HapticState
{
public:
	explicit HapticState(std::size_t historyCapacity = 256);
	XrResult Apply(std::span<const HapticDevice> devices, XrAction action, XrPath subactionPath,
		const XrHapticVibration& vibration, RuntimeTime time, bool focused);
	XrResult Stop(std::span<const HapticDevice> devices, XrAction action, RuntimeTime time, bool focused);
	XrResult Apply(std::span<const InputDeviceId> devices, XrAction action, XrPath subactionPath,
		const XrHapticVibration& vibration, RuntimeTime time, bool focused);
	XrResult Stop(std::span<const InputDeviceId> devices, XrAction action, RuntimeTime time, bool focused);
	void LoseFocus(RuntimeTime time);
	void RemoveAction(XrAction action, RuntimeTime time);
	void Reset();
	std::vector<HapticOutput> Snapshot(RuntimeTime time) const;

private:
	void Retire(std::size_t index, HapticStatus status, RuntimeTime time);
	void Expire(RuntimeTime time);
	mutable std::mutex m_mutex;
	std::size_t m_historyCapacity;
	std::vector<HapticOutput> m_active;
	std::vector<HapticOutput> m_history;
	std::uint64_t m_sequence = 0;
};
}
