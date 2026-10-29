#pragma once

#include "app/dialogs/ConnectionDialog.h"
#include "app/pages/AdvancedSteppingPage.h"
#include "app/pages/DeviceControlPanel.h"
#include "app/pages/FramePage.h"
#include "app/pages/RangePage.h"
#include "app/pages/PhysicalHeadsetPage.h"
#include "app/services/HeadsetHost.h"
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
#include <functional>
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

class RuntimePanels
{
public:
	RuntimePanels(UiRenderer& renderer, std::string endpoint = {}, Window* window = nullptr);
	~RuntimePanels();
	RuntimePanels(const RuntimePanels&) = delete;
	RuntimePanels& operator=(const RuntimePanels&) = delete;
	void ConfigureSnapshots(std::uint32_t budgetMiB);
	void ConfigurePreviews(std::uint32_t budgetMiB, std::uint32_t ramMiB);
	void ConfigurePilot(const PilotBindings& keys, const GamepadBindings& gamepad, bool remoteMouseDrag = false);
	void Poll(bool foreground = true);
	const RuntimeView& BeginFrame();
	const RuntimeView& View() const;
	bool PilotActive() const;
	bool& HudFolded();
	std::optional<ImVec4>& PreviewCrop();
	void DrawPreview(ShellState& shell);
	void DrawFramePage(ShellState& shell);
	void DrawRangePage(const RangeSel& range, ShellState& shell);
	void DrawAdvancedPage();
	void DrawDevicePage(ShellState& shell);
	void DrawSessionMenu();
	void DrawRecordingPopovers();
	std::optional<std::string> TakeSavedTake();
	void DrawConnection(bool* open = nullptr);
	void DrawWindows();
	void EndFrame();
	std::vector<Notice> TakeNotices();
	std::vector<VkSemaphore> ReadyWaits() const;
	void RecordTransfers(VkCommandBuffer command);
	void Submitted(std::uint64_t serial);
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
	std::uint64_t SnapshotChargeBytes() const;
	std::uint64_t PreviewBudget() const;
	std::uint64_t PreviewRamBytes() const;
	std::uint64_t PreviewRamBudget() const;
	std::string PreviewError() const;
	PreviewHistory& KeptPreviews();
	void LookAt(std::size_t historyIndex);
	void MarkHistoryFrame(std::size_t index, bool enabled);
	void SelectFrame(std::uint64_t local);
	void ReleaseSelected();
	void CancelPending();
	bool ReleaseInputs();
	std::vector<UndoStack::Change> Changes() const;
	void UndoChanges(std::size_t count);
	void FocusLost();
	void Deactivate();
	void BeginRetirement();
	bool RetirementComplete() const;
	void PreserveUnfinished() noexcept;
	void Notify(std::string title, std::string detail = {});
	void CancelPreviewInteraction();
	std::string Endpoint() const;
	const std::vector<std::string>& Endpoints() const;
	void RefreshEndpoints();
	void ConnectEndpoint(std::string endpoint, std::optional<std::uint64_t> expectedProcess = {});
	void SetEndpointOpenHandler(std::function<bool(const std::string&)> open);
	void Disconnect();
	// Uses the current selected run, never caller-supplied target identities.
	void ConnectHeadset(HeadsetHostOptions options);
	void SetHeadsetAdmission(std::function<bool()> available);
	void DisconnectHeadset();
	HeadsetHostSnapshot HeadsetStatus() const;
	void SetLive(bool enabled);
	void ResumeLive();
	void StopRecording();
	void StartRecording();
	void SetReplayActive(bool active);
	std::vector<std::uint64_t> CaptureIds() const;
	bool RequestExport(std::string path);
	const ImageExportTask& ImageExport() const noexcept;
	bool Exporting() const;
	void InspectSelected();
	bool CanReplay() const;
	void RequestReplay(automation::SavedRecording recording);
	std::optional<automation::SavedRecording> TakeRecordingReplay();
	void PrepareReplay();
	void FinishReplayPreparation();
	bool ReplayReady() const;

private:
	RuntimeView Build() const;
	void PollRecording();
	bool CanRecord() const;
	void PollHeadsetPreparation();
	void DrawHeadsetControls();
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

	struct HeadsetPreparation
	{
		HeadsetHostOptions options;
		PanelScope scope;
		std::optional<CommandSource> source;
		std::uint64_t observation;
		std::chrono::steady_clock::time_point deadline;
	};

	std::optional<HeadsetPreparation> m_headsetPreparation;
	RuntimeSession m_session;
	HeadsetHost m_headset;
	PhysicalHeadsetPage m_headsetPage;
	Window* m_window = nullptr;
	bool m_foreground = false;
	std::function<bool()> m_headsetAvailable;
	std::function<bool(const std::string&)> m_openEndpoint;
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
