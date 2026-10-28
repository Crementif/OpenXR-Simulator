#pragma once

#include "app/services/SessionView.h"
#include "app/services/ShellState.h"

#include "simulation/TrackingTypes.h"
#include <array>
#include <cstdint>

namespace simxr::app
{
// One UI frame's session state plus the layout and recording presentation.
struct RuntimeView : SessionView
{
	ShellRect rotationBall, rigView;
	std::array<ShellRect, MaxTrackerDevices + 3> markers{};
	// Device points in the rig view, as zero-size rectangles for hit tests and QA.
	std::array<ShellRect, MaxTrackerDevices + 3> rigDevices{};
	bool rigExpanded = false;
	bool recording = false, recordingBusy = false, recordingOverflow = false, recordingStopping = false;
	std::uint64_t recordedChanges = 0;
};
} // namespace simxr::app
