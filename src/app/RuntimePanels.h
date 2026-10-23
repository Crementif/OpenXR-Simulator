#pragma once

#include "app/services/UndoStack.h"
#include "app/services/PilotBindings.h"

#include "app/Notices.h"
#include "app/render/PreviewHistory.h"
#include "app/RuntimeView.h"
#include "app/services/SimulationEdits.h"
#include "automation/Recording.h"
#include "control/ControlSchema.h"

#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <vector>
#include <vulkan/vulkan.h>

namespace simxr::app
{
class HistoryStore;
class InputHistory;
class UiRenderer;
class Window;
// The runtime half of the companion UI: one RuntimeSession plus the controllers
// and pages that show and edit it. The shell decides where each part is drawn.
class RuntimePanels
{
  public:
	RuntimePanels(UiRenderer& renderer, std::string endpoint = {}, Window* window = nullptr);
	~RuntimePanels();
	void ConfigureSnapshots(std::uint32_t budgetMiB);
	void ConfigurePilot(const PilotBindings& keys, const GamepadBindings& gamepad);
	void Poll();
	// Builds this UI frame's view; drawers read it until the next BeginFrame.
	const RuntimeView& BeginFrame();
	const RuntimeView& View() const;
	// The pointer is captured for piloting; Esc belongs to the pilot.
	bool PilotActive() const;
	// Whether the pilot HUD is folded into a strip; the shell persists it.
	bool& HudFolded();
	// The preview's pinned crop; the shell keeps one per launch profile.
	std::optional<ImVec4>& PreviewCrop();
	void DrawPreview(ShellState& shell);
	void DrawFramePage(ShellState& shell);
	void DrawRangePage(const RangeSel& range, ShellState& shell);
	void DrawAdvancedPage();
	void DrawDevicePage(ShellState& shell);
	// Session scenarios as items of the menu being drawn.
	void DrawSessionMenu();
	// The Rec button's start-confirm and "Save take" popovers.
	void DrawRecordingPopovers();
	// A take the stop popover just saved.
	std::optional<std::string> TakeSavedTake();
	void DrawConnection(bool* open = nullptr);
	void DrawWindows();
	void EndFrame();
	std::vector<Notice> TakeNotices();
	std::vector<VkSemaphore> ReadyWaits() const;
	void RecordTransfers(VkCommandBuffer command);
	void Submitted(std::uint64_t serial);
	// A freshly built view, for automation and tests outside the UI frame.
	RuntimeView Snapshot() const;
	void Command(control::CommandKind kind, std::uint64_t capture = 0, bool raw = false, std::uint32_t layer = 0,
				 std::uint32_t view = 0);
	void Pause();
	void Step();
	std::optional<SteppedFrame> TakeSteppedFrame();
	void TogglePin(std::size_t historyIndex);
	void SelectCapture(std::uint64_t capture);
	std::vector<RuntimeFrame> TimelineFrames() const;
	const HistoryStore& History() const;
	const InputHistory& InputSamples() const;
	const PreviewHistory& KeptPreviews() const;
	PreviewHistory& KeptPreviews();
	void LookAt(std::size_t historyIndex);
	void MarkHistoryFrame(std::size_t index, bool enabled);
	void SelectFrame(std::uint64_t local);
	void ReleaseSelected();
	void CancelPending();
	void ReleaseInputs();
	// Undoable pose edits, oldest first; UndoChanges(n) returns to before the newest n.
	std::vector<UndoStack::Change> Changes() const;
	void UndoChanges(std::size_t count);
	void FocusLost();
	std::string Endpoint() const;
	const std::vector<std::string>& Endpoints() const;
	void RefreshEndpoints();
	void ConnectEndpoint(std::string endpoint);
	void Disconnect();
	void SetLive(bool enabled);
	void ResumeLive();
	void StopRecording();
	void StartRecording();
	void SetReplayActive(bool active);
	std::vector<std::uint64_t> CaptureIds() const;
	void RequestExport(std::string path);
	void InspectSelected();
	bool CanReplay() const;
	void RequestReplay(automation::SavedRecording recording);
	std::optional<automation::SavedRecording> TakeRecordingReplay();
	// A replay script needs the devices and the session's control: release the
	// companion's manual inputs and lease, then start once ReplayReady().
	void PrepareReplay();
	bool ReplayReady() const;

  private:
	class Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace simxr::app
