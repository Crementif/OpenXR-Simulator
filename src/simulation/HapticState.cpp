#include "simulation/HapticState.h"

#include <algorithm>
#include <cmath>
#include <limits>
#include <stdexcept>

namespace simxr
{
namespace
{
bool ValidDevice(InputDeviceId device)
{
	return (device.kind == InputDeviceKind::Controller && device.value < 2) ||
		(device.kind == InputDeviceKind::Tracker && device.value != 0);
}

std::vector<InputDeviceId> Controllers(std::span<const HapticDevice> devices)
{
	std::vector<InputDeviceId> result;
	for (auto device : devices)
		result.push_back(DeviceIdentity(device));
	return result;
}
}

HapticState::HapticState(std::size_t historyCapacity) : m_historyCapacity(historyCapacity)
{
	if (historyCapacity == 0)
		throw std::invalid_argument("Haptic history needs capacity");
	m_history.reserve(historyCapacity);
}

XrResult HapticState::Apply(std::span<const HapticDevice> devices, XrAction action, XrPath subactionPath,
	const XrHapticVibration& vibration, RuntimeTime time, bool focused)
{
	return Apply(Controllers(devices), action, subactionPath, vibration, time, focused);
}

XrResult HapticState::Stop(std::span<const HapticDevice> devices, XrAction action, RuntimeTime time, bool focused)
{
	return Stop(Controllers(devices), action, time, focused);
}

XrResult HapticState::Apply(std::span<const InputDeviceId> devices, XrAction action, XrPath subactionPath,
	const XrHapticVibration& vibration, RuntimeTime time, bool focused)
{
	if (vibration.type != XR_TYPE_HAPTIC_VIBRATION ||
		!std::isfinite(vibration.amplitude) || vibration.amplitude < 0 || vibration.amplitude > 1 ||
		!std::isfinite(vibration.frequency) || vibration.frequency < 0 ||
		(vibration.duration < 0 && vibration.duration != XR_MIN_HAPTIC_DURATION) || time.value.count() < 0)
		return XR_ERROR_VALIDATION_FAILURE;
	std::vector<InputDeviceId> unique;
	for (auto device : devices)
	{
		if (!ValidDevice(device))
			return XR_ERROR_VALIDATION_FAILURE;
		if (std::find(unique.begin(), unique.end(), device) == unique.end())
			unique.push_back(device);
	}
	if (!focused)
		return XR_SESSION_NOT_FOCUSED;
	std::optional<RuntimeTime> end;
	if (vibration.duration != XR_INFINITE_DURATION)
	{
		Duration duration{vibration.duration == XR_MIN_HAPTIC_DURATION ? 1000000 : vibration.duration};
		if (duration.count() > std::numeric_limits<std::int64_t>::max() - time.value.count())
			return XR_ERROR_VALIDATION_FAILURE;
		end = RuntimeTime{time.value + duration};
	}
	std::lock_guard lock(m_mutex);
	if (unique.size() > std::numeric_limits<std::uint64_t>::max() - m_sequence)
		return XR_ERROR_RUNTIME_FAILURE;
	if (unique.size() > m_active.max_size() - m_active.size())
		return XR_ERROR_OUT_OF_MEMORY;
	m_active.reserve(m_active.size() + unique.size());
	Expire(time);
	for (auto device : unique)
	{
		auto current = std::find_if(m_active.begin(), m_active.end(), [&](const auto& output) { return output.device == device; });
		if (current != m_active.end())
			Retire(static_cast<std::size_t>(current - m_active.begin()), HapticStatus::Replaced, time);
		m_active.push_back(HapticOutput{++m_sequence, device, action, subactionPath,
			vibration.amplitude, vibration.frequency, vibration.duration, time, end});
	}
	return XR_SUCCESS;
}

XrResult HapticState::Stop(std::span<const InputDeviceId> devices, XrAction action, RuntimeTime time, bool focused)
{
	if (time.value.count() < 0 || std::any_of(devices.begin(), devices.end(), [](auto device) { return !ValidDevice(device); }))
		return XR_ERROR_VALIDATION_FAILURE;
	if (!focused)
		return XR_SESSION_NOT_FOCUSED;
	std::lock_guard lock(m_mutex);
	Expire(time);
	for (std::size_t index = m_active.size(); index-- > 0;)
		if (m_active[index].action == action && std::find(devices.begin(), devices.end(), m_active[index].device) != devices.end())
			Retire(index, HapticStatus::Stopped, time);
	return XR_SUCCESS;
}

void HapticState::Retire(std::size_t index, HapticStatus status, RuntimeTime time)
{
	auto output = m_active[index];
	output.status = status;
	if (status != HapticStatus::Expired)
		output.end = std::max(time, output.start);
	if (m_history.size() == m_historyCapacity)
		m_history.erase(m_history.begin());
	m_history.push_back(output);
	m_active.erase(m_active.begin() + static_cast<std::ptrdiff_t>(index));
}

void HapticState::Expire(RuntimeTime time)
{
	for (std::size_t index = m_active.size(); index-- > 0;)
		if (m_active[index].end && *m_active[index].end <= time)
			Retire(index, HapticStatus::Expired, time);
}

void HapticState::LoseFocus(RuntimeTime time)
{
	std::lock_guard lock(m_mutex);
	Expire(time);
	while (!m_active.empty())
		Retire(m_active.size() - 1, HapticStatus::FocusLost, time);
}

void HapticState::RemoveAction(XrAction action, RuntimeTime time)
{
	std::lock_guard lock(m_mutex);
	Expire(time);
	for (std::size_t index = m_active.size(); index-- > 0;)
		if (m_active[index].action == action)
			Retire(index, HapticStatus::Stopped, time);
}

void HapticState::Reset()
{
	std::lock_guard lock(m_mutex);
	m_active.clear();
	m_history.clear();
	m_sequence = 0;
}

std::vector<HapticOutput> HapticState::Snapshot(RuntimeTime time) const
{
	std::lock_guard lock(m_mutex);
	std::vector<HapticOutput> result(m_history.begin(), m_history.end());
	for (auto value : m_active)
	{
		if (value.end && *value.end <= time)
			value.status = HapticStatus::Expired;
		result.push_back(value);
	}
	std::sort(result.begin(), result.end(), [](const auto& a, const auto& b) { return a.sequence < b.sequence; });
	return result;
}
}
