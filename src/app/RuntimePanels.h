#pragma once

#include "app/WorkspaceMode.h"
#include "automation/Recording.h"
#include "control/ControlSchema.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace simxr::app
{
class UiRenderer;
class Window;
struct RuntimePanelSnapshot
{
	bool connected = false, sessionAvailable = false, manual = false;
	bool following = true;
	bool recording = false, recordingBusy = false, recordingOverflow = false;
	std::uint64_t recordedChanges = 0;
	std::string connectionState;
	std::uint64_t session = 0, generation = 0, appliedPilotUpdates = 0;
	std::uint64_t selectedFrame = 0, selectedCapture = 0, selectedReader = 0;
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
	void Poll();
	void Draw(WorkspaceMode mode = WorkspaceMode::Simulator);
	void DrawConnection(bool* open = nullptr);
	void SetPreviewExpanded(bool expanded);
	std::vector<VkSemaphore> ReadyWaits() const;
	void Submitted(std::uint64_t serial);
	RuntimePanelSnapshot Snapshot() const;
	void Command(control::CommandKind kind, std::uint64_t capture = 0, bool raw = false, std::uint32_t layer = 0,
				 std::uint32_t view = 0);
	void SelectCapture(std::uint64_t capture);
	void ReleaseSelected();
	void CancelPending();
	void ReleaseInputs();
	std::string Endpoint() const;
	void ConnectEndpoint(std::string endpoint);
	void SetLive(bool enabled);
	void ResumeLive();
	void StopRecording();
	void SetReplayActive(bool active);
	std::vector<std::uint64_t> CaptureIds() const;
	void RequestExport(std::string path);
	std::optional<automation::SavedRecording> TakeRecordingReplay();

  private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace simxr::app
