#pragma once

#include "app/dialogs/ConnectionDialog.h"
#include "app/pages/AdvancedSteppingPage.h"
#include "app/pages/DeviceControlPanel.h"
#include "app/pages/FramePage.h"
#include "app/pages/RangePage.h"
#include "app/services/PilotBindings.h"
#include "app/services/PlayerRig.h"
#include "app/services/RuntimeSession.h"
#include "app/services/UndoStack.h"
#include "app/widgets/DocumentTree.h"
#include "app/widgets/PilotController.h"
#include "app/widgets/PreviewPanel.h"
#include "app/widgets/RecordingPanel.h"
#include "app/widgets/SpatialEditor.h"

#include "app/RuntimeView.h"
#include "app/render/PreviewHistory.h"
#include "app/services/Notice.h"
#include "app/services/SimulationEdits.h"
#include "automation/Recording.h"
#include "control/ControlSchema.h"

#include <cstddef>
#include <cstdint>
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
	RuntimePanels(const RuntimePanels&) = delete;
	RuntimePanels& operator=(const RuntimePanels&) = delete;
	void ConfigureSnapshots(std::uint32_t budgetMiB);
	void ConfigurePilot(const PilotBindings& keys, const GamepadBindings& gamepad, bool remoteMouseDrag = false);
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
	std::uint64_t PreviewBytes() const;
	std::uint64_t PreviewBudget() const;
	PreviewHistory& KeptPreviews();
	void LookAt(std::size_t historyIndex);
	void MarkHistoryFrame(std::size_t index, bool enabled);
	void SelectFrame(std::uint64_t local);
	void ReleaseSelected();
	void CancelPending();
	bool ReleaseInputs();
	// Undoable pose edits, oldest first; UndoChanges(n) returns to before the newest n.
	std::vector<UndoStack::Change> Changes() const;
	void UndoChanges(std::size_t count);
	void FocusLost();
	void CancelPreviewInteraction();
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
	bool RequestExport(std::string path);
	const ImageExportTask& ImageExport() const noexcept;
	void InspectSelected();
	bool CanReplay() const;
	void RequestReplay(automation::SavedRecording recording);
	std::optional<automation::SavedRecording> TakeRecordingReplay();
	// A replay script needs the devices and the session's control: release the
	// companion's manual inputs and lease, then start once ReplayReady().
	void PrepareReplay();
	void FinishReplayPreparation();
	bool ReplayReady() const;

  private:
	RuntimeView Build() const;
	bool CanRecord() const;
	void DrawPastDevice(const Selection& selection);
	void SubmitEdit(const SimulationEdit& edit);
	void StopPilot();

	struct RecordingReplay
	{
		automation::SavedRecording saved;
		PanelScope scope;
	};
	struct ReplayPreparation
	{
		PanelScope scope;
		std::uint64_t observation = 0;
		bool releaseQueued = false;
	};
	std::optional<ReplayPreparation> m_replayPreparation;
	RuntimeSession m_session;
	Window* m_window = nullptr;
	std::uint32_t m_gamepad = 0;
	bool m_rumbleFailed = false;
	RuntimeView m_view;
	std::string m_lastNotice, m_recordingError;
	ConnectionDialog m_connection;
	RecordingPanel m_recording;
	bool m_replayActive = false;
	std::optional<RecordingReplay> m_recordingReplay;
	std::optional<std::string> m_savedTake;
	PlayerRig m_playerRig;
	PilotController m_pilot{m_playerRig};
	DeviceControlPanel m_devices{m_playerRig};
	UndoStack m_undo;
	SpatialEditor m_spatial{m_playerRig, m_session.ManualInput(), m_undo};
	DocumentTree m_documents{m_session};
	FramePage m_frame{m_session, m_documents};
	RangePage m_range{m_session};
	AdvancedSteppingPage m_advanced{m_session, m_devices};
	PreviewPanel m_preview{m_session, m_pilot, m_spatial, m_frame, m_devices, m_window};
};
} // namespace simxr::app
