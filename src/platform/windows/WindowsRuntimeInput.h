#pragma once

#include "simulation/LegacyControllerMotion.h"
#include "simulation/TrackingTypes.h"
#include "simulation/ViewConfiguration.h"

#include <array>
#include <chrono>
#include <mutex>
#include <optional>

namespace simxr::windows
{
class WindowsRuntimeFiles;

struct RuntimeHeadPose
{
	float x = 0, y = 1.7f, z = 0;
	float yaw = 0, pitch = 0, roll = 0;
};

struct RuntimeInputSnapshot
{
	RuntimeHeadPose head;
	TrackingState tracking;
	ViewSettings views;
	std::array<ControllerControls, 2> controls;
};

class WindowsRuntimeInput
{
public:
	WindowsRuntimeInput();
	RuntimeInputSnapshot Snapshot() const;
	RuntimeInputSnapshot Poll(std::chrono::nanoseconds period, WindowsRuntimeFiles& files);
	RuntimeHeadPose Head() const;

private:
	RuntimeInputSnapshot SnapshotLocked() const;
	mutable std::mutex m_mutex;
	RuntimeHeadPose m_head;
	std::optional<std::uint32_t> m_renderWidth, m_renderHeight;
	legacy::LegacyControllerMotion m_controllers;
};
} // namespace simxr::windows
