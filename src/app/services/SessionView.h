#pragma once

#include "app/services/FrameIdentity.h"
#include "app/services/SimulationEdits.h"
#include "graphics/FrameOutput.h"

#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace simxr::app
{
// Following shows the live edge; otherwise the exact retained frame at the cursor.
struct TimeCursor
{
	bool following = true;
	FrameIdentity cursor;
	std::uint64_t epoch = 0;
};
struct RuntimeFrameRun
{
	std::uint64_t epoch = 0, session = 0, generation = 0;
	bool operator==(const RuntimeFrameRun&) const = default;
};
struct RuntimeFrame
{
	RuntimeFrameRun run;
	std::uint64_t local = 0, capture = 0, frame = 0;
	bool selected = false, imageReady = false, stateReady = false, remoteAvailable = false;
};
// A frame that Next frame (or following while paused) moved to.
struct SteppedFrame
{
	FrameIdentity frame;
	std::uint64_t epoch = 0;
	// Next frame asked for it; otherwise it only updates an open Frame page.
	bool requested = false;
};
// Read-only state of one runtime connection.
struct SessionView
{
	TimeCursor time;
	// The latest applied device state and input owners, once the runtime reported one.
	std::optional<ObservedSetup> setup;
	std::vector<FrameView> selectedViews;
	double historySeconds = 0;
	// The loaded image (or capture) the preview currently shows.
	FrameIdentity selectedIdentity;
	bool connected = false, sessionAvailable = false, sessionRunning = false, manual = false;
	bool historyFull = false, selectedPinned = false;
	std::string connectionState;
	std::uint64_t epoch = 0, session = 0, generation = 0, appliedPilotUpdates = 0, latestFrame = 0;
	std::uint64_t selectedEpoch = 0, selectedFrame = 0, selectedCapture = 0, selectedReader = 0, inspectedFrame = 0,
				  selectedImageCapture = 0;
	std::size_t images = 0, captures = 0;
	bool pending = false;
	bool operationPending = false, selectedReady = false;
	std::uint64_t lastOperation = 0, lastCapture = 0;
	std::uint32_t selectedLayers = 0;
	float fps = 0;
	std::string phase, progress, endpoint, appName;
	// A failure stops the session (connection lost, protocol error); a notice does not.
	std::string error, notice;
};
} // namespace simxr::app
