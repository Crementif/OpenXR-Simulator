#pragma once

#include "app/ShellState.h"
#include "app/services/SessionView.h"

#include <array>
#include <cstdint>

namespace simxr::app
{
// One UI frame's session state plus the layout and recording presentation.
struct RuntimeView : SessionView
{
	ShellRect rotationBall, rigView;
	std::array<ShellRect, 7> markers{};
	// Device points in the rig view, as zero-size rectangles for hit tests and QA.
	std::array<ShellRect, 7> rigDevices{};
	bool rigExpanded = false;
	bool recording = false, recordingBusy = false, recordingOverflow = false;
	std::uint64_t recordedChanges = 0;
};
} // namespace simxr::app
