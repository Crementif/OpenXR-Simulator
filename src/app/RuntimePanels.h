#pragma once
#include "app/PilotBindings.h"

#include "app/ShellState.h"
#include "automation/Recording.h"
#include "control/ControlSchema.h"
#include "graphics/FrameOutput.h"

#include <array>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace simxr::app
{
class HistoryStore;
class UiRenderer;
class Window;
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
struct RuntimePanelSnapshot
{
	std::vector<FrameView> selectedViews;
	double historySeconds = 0;
	FrameIdentity selectedIdentity;
	ShellRect rotationBall, reachMap;
	std::array<ShellRect, 7> markers{};
	bool connected = false, sessionAvailable = false, manual = false;
	bool following = true, rigExpanded = false;
	bool historyFull = false, selectedPinned = false;
	bool recording = false, recordingBusy = false, recordingOverflow = false;
	std::uint64_t recordedChanges = 0;
	std::string connectionState;
	std::uint64_t epoch = 0, session = 0, generation = 0, appliedPilotUpdates = 0, latestFrame = 0;
	std::uint64_t selectedFrame = 0, selectedCapture = 0, selectedReader = 0, inspectedFrame = 0,
				  selectedImageCapture = 0;
	std::size_t images = 0, captures = 0;
	bool pending = false;
	bool operationPending = false, selectedReady = false;
	std::uint64_t lastOperation = 0, lastCapture = 0;
	std::uint32_t selectedLayers = 0;
	std::string phase, progress, endpoint;
	std::string error;
};
class RuntimePanels
{
  public:
	RuntimePanels(UiRenderer& renderer, std::string endpoint = {}, Window* window = nullptr);
	~RuntimePanels();
	void ConfigureSnapshots(std::uint32_t budgetMiB);
	void ConfigurePilot(const PilotBindings& keys, const GamepadBindings& gamepad);
	void Poll();
	void Draw(ShellState& shell);
	void DrawConnection(bool* open = nullptr);
	void SetPreviewExpanded(bool expanded);
	std::vector<VkSemaphore> ReadyWaits() const;
	void Submitted(std::uint64_t serial);
	RuntimePanelSnapshot Snapshot() const;
	void Command(control::CommandKind kind, std::uint64_t capture = 0, bool raw = false, std::uint32_t layer = 0,
				 std::uint32_t view = 0);
	void SelectCapture(std::uint64_t capture);
	std::vector<RuntimeFrame> TimelineFrames() const;
	const HistoryStore& History() const;
	void SelectHistoryFrame(std::size_t index);
	void SelectFrame(std::uint64_t local);
	void ReleaseSelected();
	void CancelPending();
	void ReleaseInputs();
	void FocusLost();
	std::string Endpoint() const;
	void ConnectEndpoint(std::string endpoint);
	void SetLive(bool enabled);
	void ResumeLive();
	void StopRecording();
	void StartRecording();
	void SetReplayActive(bool active);
	std::vector<std::uint64_t> CaptureIds() const;
	void RequestExport(std::string path);
	void InspectSelected();
	std::optional<automation::SavedRecording> TakeRecordingReplay();

  private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace simxr::app
