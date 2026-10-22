#include "app/RuntimePanels.h"
#include "app/AdvancedSteppingPage.h"
#include "app/DeviceControlPanel.h"
#include "app/DocumentTree.h"
#include "app/FramePage.h"
#include "app/PilotController.h"
#include "app/PreviewPanel.h"
#include "app/RangePage.h"
#include "app/RecordingPanel.h"
#include "app/RuntimeSession.h"
#include "app/SpatialEditor.h"
#include "app/Theme.h"
#include "app/Window.h"
#include "app/widgets/Controls.h"

#include <algorithm>
#include <array>
#include <filesystem>
#include <imgui.h>
#include <span>
#include <utility>

namespace simxr::app
{
namespace
{
namespace wire = control;
namespace tag = wire::tag;
} // namespace
class RuntimePanels::Impl
{
  public:
	Impl(UiRenderer& renderer, const std::string& endpoint, Window* window)
		: m_session(renderer, endpoint), m_window(window)
	{
		std::copy_n(endpoint.data(), std::min(endpoint.size(), m_endpointInput.size() - 1), m_endpointInput.data());
	}
	~Impl()
	{
		StopPilot();
	}
	void Poll()
	{
		m_session.Poll(m_pilot.Active());
		if (m_session.TakeScopeChanged())
		{
			StopPilot();
			m_devices.ClearHeldInputs();
			m_devices.ResetObservations();
			m_spatial.Reset();
		}
		if (m_session.Connected())
		{
			const auto& input = m_session.Input();
			m_spatial.Observe(input, m_pilot.Active());
			m_devices.ObserveDelivery(input.editPending, input.error);
			m_devices.SetManualSource(input.source);
			m_devices.SetRigPiloting(m_pilot.Active() || m_spatial.Active());
			if (m_session.TakeObservation())
				m_devices.ObserveState(input.state);
			if (!input.error.empty())
				StopPilot();
		}
		m_range.Poll();
	}
	RuntimeView Build() const
	{
		auto view = m_session.View();
		const auto recording = m_recording.Snapshot();
		view.recording = recording.page && recording.page->active;
		view.recordingOverflow = recording.page && recording.page->overflow;
		view.recordingBusy = recording.busy;
		view.recordedChanges = recording.page ? recording.page->total : 0;
		view.rigExpanded = m_spatial.Expanded();
		view.rotationBall = m_spatial.RotationBallRect();
		view.reachMap = m_spatial.ReachMapRect();
		view.markers = m_spatial.MarkerRects();
		if (view.notice.empty())
			view.notice = m_lastNotice;
		return view;
	}
	bool CanRecord() const
	{
		return m_session.Connected() && m_session.Debugger().Selected().id &&
			   m_session.Supports(wire::Capability::CommandRecording);
	}
	const RuntimeView& BeginFrame()
	{
		const auto connection = m_session.Connection();
		const auto selected = m_session.Debugger().Selected();
		const bool connected = m_session.Connected();
		m_recording.SetTarget(connected && selected.id
								  ? RecordingTarget{connection.path, selected.id, selected.generation}
								  : RecordingTarget{});
		m_recording.Poll(CanRecord());
		m_view = Build();
		const bool following = m_view.time.following;
		const auto image = m_session.Preview().Selected();
		const auto historical = m_session.Workspace().Selected();
		m_spatial.SetContext(connected && selected.id && following && !m_pilot.Active(), !following,
							 image ? std::span<const FrameView>{image->description.views} : std::span<const FrameView>{},
							 historical && !following ? &historical->document : nullptr,
							 historical ? historical->local : 0);
		m_session.Preview().BeginFrame();
		return m_view;
	}
	void DrawPreview(ShellState& shell)
	{
		if (!m_preview.Draw(shell, m_view))
			StopPilot();
	}
	void DrawDevicePage(ShellState& shell)
	{
		if (!m_view.connected)
		{
			const auto available = ImGui::GetContentRegionAvail();
			const auto origin = ImGui::GetCursorScreenPos();
			const float unit = ImGui::GetFontSize() / 17;
			const char* text = "No app connected";
			const auto size = ImGui::CalcTextSize(text);
			DrawIcon(ImGui::GetWindowDrawList(), Icon::Headset, {origin.x + available.x / 2, origin.y + 60 * unit},
					 36 * unit, theme::TextDim);
			ImGui::SetCursorScreenPos({origin.x + (available.x - size.x) / 2, origin.y + 90 * unit});
			ImGui::TextDisabled("%s", text);
			const float width = ImGui::CalcTextSize("Open app").x + ImGui::GetFrameHeight() + ImGui::GetStyle().FramePadding.x;
			ImGui::SetCursorScreenPos({origin.x + (available.x - width) / 2, ImGui::GetCursorScreenPos().y + 4 * unit});
			if (IconButton("Open app##Inspector", Icon::App, "Launch or connect an OpenXR app", false, "Open app"))
				shell.openAppPicker = true;
			return;
		}
		m_spatial.DrawInspector(shell.selection);
		m_devices.SetRigPiloting(m_pilot.Active() || m_spatial.Active());
		if (!m_view.time.following)
		{
			// Looking back: the rig above shows the recorded poses; live controls stay hidden.
			ImGui::TextDisabled("Frame %llu", static_cast<unsigned long long>(m_view.time.cursor.frame));
			if (ImGui::SmallButton("Frame details"))
				shell.selection = FrameSel{m_view.time.cursor, m_view.time.epoch};
			return;
		}
		if (auto edit = m_devices.Draw(m_session.SupportsEdits(), &shell.selection))
		{
			StopPilot();
			m_session.SubmitEdit(*edit);
		}
		if (auto device = std::get_if<DeviceSel>(&shell.selection); device && device->page == DevicePage::Trackers)
			device->slot = static_cast<unsigned>(m_devices.SelectedTracker());
	}
	void DrawLibrary()
	{
#ifdef SIMXR_COMPANION_AUTOMATION
		const bool replayAvailable = CanRecord() && !m_replayActive;
#else
		constexpr bool replayAvailable = false;
#endif
		if (auto replay = m_recording.Draw(CanRecord(), replayAvailable, m_replayActive))
			m_recordingReplay = RecordingReplay{std::move(*replay), m_session.Debugger().Scope()};
	}
	void DrawConnection(bool* open)
	{
		const auto selected = m_session.Debugger().Selected();
		ImGui::SetNextWindowSize({600, 400}, ImGuiCond_FirstUseEver);
		ImGui::OpenPopup("Runtime connection");
		if (!ImGui::BeginPopupModal("Runtime connection", open))
			return;
		const auto state = m_session.Connection();
		ImGui::TextUnformatted(m_view.connectionState.c_str());
		if (ImGui::Button("Refresh applications"))
			m_session.RefreshEndpoints();
		if (state.state == RuntimeConnectionState::Connected)
		{
			ImGui::SameLine();
			if (ImGui::Button("Disconnect"))
				m_session.Disconnect();
		}
		if (m_session.Endpoints().empty())
			ImGui::TextUnformatted("No applications");
		for (const auto& path : m_session.Endpoints())
		{
			if (ImGui::Selectable(std::filesystem::path(path).filename().string().c_str(),
								  path == m_session.Endpoint()))
				m_session.ConnectEndpoint(path);
			ImGui::SetItemTooltip("%s", path.c_str());
		}
		if (!state.detail.empty())
			ImGui::TextWrapped("%s", state.detail.c_str());
		for (const auto& session : m_session.Debugger().Sessions())
		{
			const auto label = "Session " + std::to_string(session.id) + "##" + std::to_string(session.generation);
			if (ImGui::Selectable(label.c_str(), session.id == selected.id && session.generation == selected.generation))
				m_session.ChooseSession(session);
			ImGui::SetItemTooltip("Run %llu", static_cast<unsigned long long>(session.generation));
		}
		if (ImGui::CollapsingHeader("Advanced connection"))
		{
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##Endpoint", m_endpointInput.data(), m_endpointInput.size());
			if (ImGui::Button("Connect to endpoint"))
				m_session.ConnectEndpoint(m_endpointInput.data());
			ImGui::TextWrapped("Queued: %zu; replies: %zu; exchange: %s", state.queued, state.ready,
							   state.inFlight ? "pending" : "idle");
		}
		ImGui::EndPopup();
	}
	std::vector<Notice> TakeNotices()
	{
		auto notices = m_session.TakeNotices();
		if (!notices.empty())
			m_lastNotice = notices.back().title + (notices.back().detail.empty() ? "" : ": " + notices.back().detail);
		return notices;
	}
	void StopPilot()
	{
		m_pilot.Stop(m_window, m_session.ManualInput(), true);
		m_devices.SetRigPiloting(false);
	}
	void FocusLost()
	{
		m_pilot.Stop(m_window, m_session.ManualInput(), true, true);
		m_devices.SetRigPiloting(false);
		m_devices.ClearHeldInputs();
	}
	void ReleaseInputs()
	{
		StopPilot();
		m_devices.ClearHeldInputs();
		if (m_session.Debugger().Selected().id && m_session.Debugger().Selected().running)
			m_session.SubmitEdit(ReleaseManualInputs{});
	}
	void RequestExport(std::string path)
	{
		if (m_range.Exporting())
			m_session.Notify("Export busy", "Wait for the range export to finish");
		else
			m_session.RequestExport(std::move(path));
	}
	std::optional<automation::SavedRecording> TakeRecordingReplay()
	{
		auto replay = std::exchange(m_recordingReplay, {});
		if (!replay)
			return {};
		if (replay->scope != m_session.Debugger().Scope())
		{
			m_session.Notify("Replay target changed", "Select the intended session and replay again");
			return {};
		}
		return std::move(replay->saved);
	}

	struct RecordingReplay
	{
		automation::SavedRecording saved;
		PanelScope scope;
	};
	RuntimeSession m_session;
	Window* m_window = nullptr;
	RuntimeView m_view;
	std::string m_lastNotice;
	std::array<char, 256> m_endpointInput{};
	RecordingPanel m_recording;
	bool m_replayActive = false;
	std::optional<RecordingReplay> m_recordingReplay;
	PlayerRig m_playerRig;
	PilotController m_pilot{m_playerRig};
	DeviceControlPanel m_devices{m_playerRig};
	SpatialEditor m_spatial{m_playerRig, m_session.ManualInput()};
	DocumentTree m_documents{m_session};
	FramePage m_frame{m_session, m_spatial, m_documents};
	RangePage m_range{m_session};
	AdvancedSteppingPage m_advanced{m_session, m_devices};
	PreviewPanel m_preview{m_session, m_pilot, m_spatial, m_devices, m_window};
};
RuntimePanels::RuntimePanels(UiRenderer& renderer, std::string endpoint, Window* window)
	: m_impl(std::make_unique<Impl>(renderer, endpoint, window))
{
}
RuntimePanels::~RuntimePanels() = default;
void RuntimePanels::ConfigureSnapshots(std::uint32_t budgetMiB)
{
	m_impl->m_session.ConfigureSnapshots(budgetMiB);
}
void RuntimePanels::ConfigurePilot(const PilotBindings& keys, const GamepadBindings& gamepad)
{
	m_impl->m_pilot.Configure(keys, gamepad);
}
void RuntimePanels::Poll()
{
	m_impl->Poll();
}
const RuntimeView& RuntimePanels::BeginFrame()
{
	return m_impl->BeginFrame();
}
const RuntimeView& RuntimePanels::View() const
{
	return m_impl->m_view;
}
void RuntimePanels::DrawPreview(ShellState& shell)
{
	m_impl->DrawPreview(shell);
}
void RuntimePanels::DrawFramePage(ShellState& shell)
{
	m_impl->m_frame.Draw(shell, m_impl->m_view);
}
void RuntimePanels::DrawRangePage(const RangeSel& range, ShellState& shell)
{
	auto& impl = *m_impl;
	if (auto replay = impl.m_range.Draw(range, shell))
		impl.m_recordingReplay = Impl::RecordingReplay{std::move(*replay), impl.m_session.Debugger().Scope()};
	if (const auto saved = impl.m_range.TakeSaved())
		impl.m_recording.Added(*saved);
}
void RuntimePanels::DrawAdvancedPage()
{
	m_impl->m_advanced.Draw();
}
void RuntimePanels::DrawDevicePage(ShellState& shell)
{
	m_impl->DrawDevicePage(shell);
}
void RuntimePanels::DrawLibrary()
{
	m_impl->DrawLibrary();
}
void RuntimePanels::DrawConnection(bool* open)
{
	m_impl->DrawConnection(open);
}
void RuntimePanels::DrawWindows()
{
	m_impl->m_documents.DrawWindow();
}
std::vector<Notice> RuntimePanels::TakeNotices()
{
	return m_impl->TakeNotices();
}
std::vector<VkSemaphore> RuntimePanels::ReadyWaits() const
{
	return m_impl->m_session.Preview().ReadyWaits();
}
void RuntimePanels::Submitted(std::uint64_t serial)
{
	m_impl->m_session.Preview().Submitted(serial);
}
RuntimeView RuntimePanels::Snapshot() const
{
	return m_impl->Build();
}
void RuntimePanels::Command(control::CommandKind kind, std::uint64_t capture, bool raw, std::uint32_t layer,
							std::uint32_t view)
{
	m_impl->m_session.Command(kind, capture, raw, layer, view);
}
void RuntimePanels::Pause()
{
	m_impl->m_session.Pause();
}
void RuntimePanels::Step()
{
	m_impl->m_session.Step();
}
std::optional<SteppedFrame> RuntimePanels::TakeSteppedFrame()
{
	return m_impl->m_session.TakeSteppedFrame();
}
void RuntimePanels::TogglePin(std::size_t index)
{
	m_impl->m_session.TogglePin(index);
}
void RuntimePanels::MarkHistoryFrame(std::size_t index, bool enabled)
{
	m_impl->m_session.MarkHistoryFrame(index, enabled);
}
void RuntimePanels::LookAt(std::size_t index)
{
	m_impl->m_session.LookAt(index);
}
const HistoryStore& RuntimePanels::History() const
{
	return m_impl->m_session.History();
}
std::vector<RuntimeFrame> RuntimePanels::TimelineFrames() const
{
	return m_impl->m_session.TimelineFrames();
}
void RuntimePanels::SelectFrame(std::uint64_t local)
{
	m_impl->m_session.SelectFrame(local);
}
void RuntimePanels::SelectCapture(std::uint64_t capture)
{
	m_impl->m_session.SelectCapture(capture);
}
void RuntimePanels::ReleaseSelected()
{
	m_impl->m_session.ReleaseSelected();
}
void RuntimePanels::CancelPending()
{
	m_impl->m_session.CancelPending();
}
void RuntimePanels::ReleaseInputs()
{
	m_impl->ReleaseInputs();
}
void RuntimePanels::FocusLost()
{
	m_impl->FocusLost();
}
std::string RuntimePanels::Endpoint() const
{
	return m_impl->m_session.Endpoint();
}
const std::vector<std::string>& RuntimePanels::Endpoints() const
{
	return m_impl->m_session.Endpoints();
}
void RuntimePanels::RefreshEndpoints()
{
	m_impl->m_session.RefreshEndpoints();
}
void RuntimePanels::ConnectEndpoint(std::string endpoint)
{
	m_impl->m_session.ConnectEndpoint(std::move(endpoint));
}
void RuntimePanels::Disconnect()
{
	m_impl->m_session.Disconnect();
}
void RuntimePanels::StartRecording()
{
	m_impl->m_recording.RequestStart();
}
void RuntimePanels::StopRecording()
{
	m_impl->m_recording.Stop();
}
void RuntimePanels::SetReplayActive(bool active)
{
	m_impl->m_replayActive = active;
}
void RuntimePanels::ResumeLive()
{
	m_impl->m_session.ResumeLive();
}
void RuntimePanels::SetLive(bool enabled)
{
	m_impl->m_session.SetLive(enabled);
}
std::vector<std::uint64_t> RuntimePanels::CaptureIds() const
{
	return m_impl->m_session.Workspace().CaptureIds();
}
void RuntimePanels::InspectSelected()
{
	m_impl->m_session.Workspace().InspectSelected();
}
void RuntimePanels::RequestExport(std::string path)
{
	m_impl->RequestExport(std::move(path));
}
void RuntimePanels::PrepareReplay()
{
	m_impl->ReleaseInputs();
	m_impl->m_session.Debugger().Relinquish();
}
bool RuntimePanels::ReplayReady() const
{
	const auto& debugger = m_impl->m_session.Debugger();
	const auto& setup = m_impl->m_session.Setup();
	return !debugger.HasLease() && !debugger.Relinquishing() &&
		   !(setup && std::ranges::any_of(setup->owners,
										  [](const std::optional<CommandSource>& owner)
										  {
											  return owner && owner->kind == CommandSourceKind::Manual;
										  }));
}
std::optional<automation::SavedRecording> RuntimePanels::TakeRecordingReplay()
{
	return m_impl->TakeRecordingReplay();
}
} // namespace simxr::app
