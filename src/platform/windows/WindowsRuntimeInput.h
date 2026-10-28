#pragma once

#include "control/legacy/WindowsLegacyCommands.h"
#include "simulation/LegacyControllerMotion.h"
#include "simulation/TrackingTypes.h"
#include "simulation/ViewConfiguration.h"

#include <array>
#include <chrono>
#include <mutex>
#include <optional>

namespace mcp
{
class WindowsLegacyChannel;
}

namespace simxr::windows
{
struct RuntimeInputSnapshot
{
	mcp::HeadPoseCommand head;
	TrackingState tracking;
	ViewSettings views;
	std::array<ControllerControls, 2> controls;
};

class WindowsRuntimeInput
{
  public:
	WindowsRuntimeInput();
	RuntimeInputSnapshot Snapshot() const;
	RuntimeInputSnapshot Poll(std::chrono::nanoseconds period, mcp::WindowsLegacyChannel& files);
	mcp::HeadPoseCommand Head() const;
	void SetHead(const mcp::HeadPoseCommand& pose);

  private:
	RuntimeInputSnapshot SnapshotLocked(double uptimeSeconds) const;
	mutable std::mutex m_mutex;
	mcp::HeadPoseCommand m_head;
	std::optional<std::array<XrFovf, 2>> m_fov;
	std::optional<float> m_ipd;
	std::optional<std::uint32_t> m_renderWidth, m_renderHeight;
	mcp::PoseSweepCommand m_sweep;
	double m_sweepStart = 0;
	legacy::LegacyControllerMotion m_controllers;
};
} // namespace simxr::windows
