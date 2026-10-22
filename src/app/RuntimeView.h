#pragma once

#include "app/ShellState.h"
#include "app/SimulationEdits.h"
#include "graphics/FrameOutput.h"

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace simxr::app
{
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
	FrameSel frame;
	// Next frame asked for it; otherwise it only updates an open Frame page.
	bool requested = false;
};
// One read-only picture of the runtime, built once per UI frame for drawers.
struct RuntimeView
{
	TimeCursor time;
	// The latest applied device state and input owners, once the runtime reported one.
	std::optional<ObservedSetup> setup;
	std::vector<FrameView> selectedViews;
	double historySeconds = 0;
	// The loaded image (or capture) the preview currently shows.
	FrameIdentity selectedIdentity;
	ShellRect rotationBall, reachMap;
	std::array<ShellRect, 7> markers{};
	bool connected = false, sessionAvailable = false, sessionRunning = false, manual = false;
	bool rigExpanded = false;
	bool historyFull = false, selectedPinned = false;
	bool recording = false, recordingBusy = false, recordingOverflow = false;
	std::uint64_t recordedChanges = 0;
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
	std::string phase, progress, endpoint;
	// A failure stops the session (connection lost, protocol error); a notice does not.
	std::string error, notice;
};
} // namespace simxr::app
