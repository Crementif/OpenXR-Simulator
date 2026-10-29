#pragma once

#include "app/render/PreviewImages.h"
#include "app/services/DebuggerPanelController.h"
#include "app/services/FrameHistoryFeed.h"
#include "app/services/HapticFeedback.h"
#include "app/services/HistoryCoverage.h"
#include "app/services/ImageExportTask.h"
#include "app/services/InspectionWorkspace.h"
#include "app/services/ManualInputController.h"
#include "app/services/Notice.h"
#include "app/services/SessionView.h"

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace simxr::app
{
class UiRenderer;

class RuntimeSession
{
public:
	RuntimeSession(UiRenderer& renderer, const std::string& endpoint);
	~RuntimeSession();
	RuntimeSession(const RuntimeSession&) = delete;
	RuntimeSession& operator=(const RuntimeSession&) = delete;

	void Poll(bool piloting, bool foreground = true);
	SessionView View() const;
	bool TakeScopeChanged();
	std::vector<Notice> TakeNotices();
	void Notify(std::string title, std::string detail = {});
	void Notify(Notice notice);

	void Command(control::CommandKind kind, std::uint64_t capture = 0, bool raw = false, std::uint32_t layer = 0,
				 std::uint32_t view = 0);
	void Pause();
	void Step();
	void ResumeLive();
	void SetLive(bool enabled);
	std::optional<SteppedFrame> TakeSteppedFrame();
	void CancelPending();

	void LookAt(std::size_t historyIndex);
	bool LoadingPastImage() const;
	const control::Message* PastState() const;

	std::uint64_t PreviewBytes() const;
	std::uint64_t PreviewBudget() const;
	std::uint64_t PreviewRamBytes() const;
	std::uint64_t PreviewRamBudget() const;
	std::string PreviewError() const;
	void SelectFrame(std::uint64_t local);
	void SelectCapture(std::uint64_t capture);
	void SelectImage(std::uint64_t image);
	void ReleaseSelected();
	void TogglePin(std::size_t historyIndex);
	void MarkHistoryFrame(std::size_t historyIndex, bool enabled);
	bool RequestExport(std::string path);

	void ConnectEndpoint(std::string endpoint, std::optional<std::uint64_t> expectedProcess = {});
	void Disconnect();
	void RefreshEndpoints();

	const std::vector<std::string>& Endpoints() const noexcept
	{
		return m_endpoints;
	}

	std::string Endpoint() const;
	void ChooseSession(PanelSession session);
	void ConfigureSnapshots(std::uint32_t budgetMiB);
	void ConfigurePreviews(std::uint32_t budgetMiB, std::uint32_t ramMiB);
	bool SubmitEdit(const SimulationEdit& edit);

	bool Connected() const;
	bool SupportsEdits() const;
	bool Supports(control::Capability capability) const;

	std::uint64_t Epoch() const noexcept
	{
		return m_requests.Epoch();
	}

	const TimeCursor& Time() const noexcept
	{
		return m_time;
	}

	RuntimeConnectionSnapshot Connection() const
	{
		return m_requests.Snapshot();
	}

	std::vector<RuntimeFrame> TimelineFrames() const;

	const HistoryStore& History() const
	{
		return m_history.History();
	}

	const InputHistory& InputSamples() const
	{
		return m_history.Input();
	}

	const std::optional<ObservedSetup>& Setup() const noexcept
	{
		return m_setup;
	}

	DebuggerPanelController& Debugger() noexcept
	{
		return m_debugger;
	}

	const DebuggerPanelController& Debugger() const noexcept
	{
		return m_debugger;
	}

	InspectionWorkspace& Workspace() noexcept
	{
		return m_workspace;
	}

	const InspectionWorkspace& Workspace() const noexcept
	{
		return m_workspace;
	}

	PreviewImages& Preview() noexcept
	{
		return m_preview;
	}

	const PreviewImages& Preview() const noexcept
	{
		return m_preview;
	}

	ImageExportTask& Exports() noexcept
	{
		return m_exports;
	}

	const ImageExportTask& Exports() const noexcept
	{
		return m_exports;
	}

	ManualInputController& ManualInput() noexcept
	{
		return m_manualInput;
	}

	const ManualInputSnapshot& Input() const noexcept
	{
		return m_input;
	}

	bool TakeObservation();

	HapticFeedback& Haptics() noexcept
	{
		return m_haptics;
	}

	bool AutoSelect() const noexcept
	{
		return m_autoSelect;
	}

	void SetAutoSelect(bool enabled) noexcept
	{
		m_autoSelect = enabled;
	}

private:
	using Clock = std::chrono::steady_clock;
	void Reply(const PanelReply& response);
	void ScopeChanged();
	void ShowCapture(std::uint64_t capture);
	RuntimePanelRequests m_requests;
	DebuggerPanelController m_debugger;
	InspectionWorkspace m_workspace;
	PreviewImages m_preview;
	ImageExportTask m_exports;
	FrameHistoryFeed m_history;
	ManualInputController m_manualInput;
	ManualInputSnapshot m_input;
	HapticFeedback m_haptics;
	TimeCursor m_time;
	control::Message m_pastState;
	std::optional<SteppedFrame> m_stepped;
	std::optional<ObservedSetup> m_setup;
	HistoryCoverage m_coverage;
	std::vector<Notice> m_notices;
	std::vector<std::string> m_endpoints;
	std::string m_endpoint, m_failure, m_historyNotice, m_hapticFailure;
	PanelScope m_budgetScope;
	std::uint32_t m_budgetMiB = 0;
	Clock::time_point m_nextHistory{}, m_nextPreview{}, m_nextCapture{}, m_nextHaptics{}, m_hapticsRequested{},
		m_nextBudget{}, m_nextPreviewBudget{};
	std::uint64_t m_unavailablePreview = 0;
	std::uint64_t m_observation = 0, m_resumeAfter = 0;
	bool m_autoSelect = true, m_followOnResume = false, m_stepRequested = false, m_scopeChanged = false,
		 m_observationChanged = false;
};
} // namespace simxr::app
