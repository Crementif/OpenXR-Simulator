#include "simulation/InputState.h"
#include "Pch.h"
#include "runtime/validation/Path.h"

#include <cmath>
#include <limits>
#include <set>

namespace simxr
{
XrResult InputState::Validate(const InputSnapshot& snapshot)
{
	if (snapshot.time.value.count() < 0)
		return XR_ERROR_TIME_INVALID;
	auto validControls = [](const ControllerControls& controller)
	{
		for (float value : {controller.trigger, controller.grip, controller.gripForce, controller.trackpadForce})
			if (!std::isfinite(value) || value < 0 || value > 1)
				return false;
		for (float value : {controller.stick.x, controller.stick.y, controller.trackpad.x, controller.trackpad.y})
			if (!std::isfinite(value) || value < -1 || value > 1)
				return false;
		return true;
	};
	for (const auto& controller : snapshot.controllers)
		if (!validControls(controller))
			return XR_ERROR_VALIDATION_FAILURE;
	for (std::size_t index = 0; index < snapshot.devices.size(); ++index)
	{
		const auto& device = snapshot.devices[index];
		if ((device.identity.kind != InputDeviceKind::EyeGaze && device.identity.kind != InputDeviceKind::Tracker) ||
			(device.identity.kind == InputDeviceKind::Tracker && !device.identity.value) ||
			(device.identity.kind == InputDeviceKind::EyeGaze && device.identity.value) || device.userPaths.empty() ||
			!validControls(device.controls))
			return XR_ERROR_VALIDATION_FAILURE;
		std::set<std::string_view> uniquePaths;
		for (const auto& path : device.userPaths)
		{
			if (!IsValidXrPath(path) || !path.starts_with("/user/"))
				return XR_ERROR_PATH_FORMAT_INVALID;
			if (!uniquePaths.insert(path).second ||
				(device.identity.kind == InputDeviceKind::EyeGaze && path != "/user/eyes_ext") ||
				(device.identity.kind == InputDeviceKind::Tracker && !path.starts_with("/user/vive_tracker_htcx/")))
				return XR_ERROR_VALIDATION_FAILURE;
			for (std::size_t previous = 0; previous < index; ++previous)
			{
				const auto& other = snapshot.devices[previous];
				if (other.identity == device.identity ||
					(device.connected && other.connected &&
					 std::find(other.userPaths.begin(), other.userPaths.end(), path) != other.userPaths.end()))
					return XR_ERROR_VALIDATION_FAILURE;
			}
		}
	}
	return XR_SUCCESS;
}

XrResult InputState::Publish(InputSnapshot snapshot)
{
	auto result = Validate(snapshot);
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(m_mutex);
	if (snapshot.time < m_snapshot.time)
		return XR_ERROR_TIME_INVALID;
	if (m_snapshot.revision == std::numeric_limits<std::uint64_t>::max())
		return XR_ERROR_RUNTIME_FAILURE;
	snapshot.revision = m_snapshot.revision + 1;
	m_snapshot = std::move(snapshot);
	return XR_SUCCESS;
}

InputSnapshot InputState::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}

std::uint64_t InputState::Revision() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot.revision;
}

void InputState::Reset()
{
	std::lock_guard lock(m_mutex);
	m_snapshot = {};
}
void InputState::ReleaseControls(std::array<bool, 2> hands)
{
	if (!hands[0] && !hands[1])
		return;
	std::lock_guard lock(m_mutex);
	for (std::size_t hand = 0; hand < hands.size(); ++hand)
		if (hands[hand])
		{
			const bool connected = m_snapshot.controllers[hand].connected;
			m_snapshot.controllers[hand] = {};
			m_snapshot.controllers[hand].connected = connected;
		}
	if (m_snapshot.revision != std::numeric_limits<std::uint64_t>::max())
		++m_snapshot.revision;
}

} // namespace simxr
