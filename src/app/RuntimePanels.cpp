#include "app/RuntimePanels.h"
#include "Pch.h"
#include "app/platform/Window.h"
#include "app/widgets/Controls.h"
#include "app/widgets/HapticEvents.h"
#include "app/widgets/PastInput.h"
#include "app/widgets/Theme.h"
#include <imgui.h>

namespace simxr::app
{
namespace wire = control;
void RuntimePanels::Poll()
{
	m_session.Poll(m_pilot.Active());
	if (m_session.TakeScopeChanged())
	{
		StopPilot();
		m_preview.CancelInteraction();
		m_devices.ClearHeldInputs();
		m_devices.ResetObservations();
		m_spatial.Reset();
		m_undo.Clear();
		m_replayPreparation.reset();
	}
	if (m_replayPreparation && m_session.Debugger().HasLease())
		m_session.Debugger().Relinquish();
	if (m_session.Connected())
	{
		const auto& input = m_session.Input();
		m_devices.ObserveDelivery(input.editPending, input.error);
		m_spatial.Observe(input, m_pilot.Active() || m_devices.Active());
		m_devices.SetManualSource(input.source);
		m_devices.SetRigPiloting(m_pilot.Active() || m_spatial.Active() || m_devices.Active());
		if (m_session.TakeObservation())
		{
			m_devices.ObserveState(input.state);
			if (!input.editPending && !input.posePending && m_session.Setup())
				m_undo.Observe(*m_session.Setup());
		}
		if (!input.error.empty())
			StopPilot();
	}
	if (m_window)
	{
		if (m_gamepad != m_window->GamepadIdentity())
		{
			m_gamepad = m_window->GamepadIdentity();
			m_rumbleFailed = false;
			m_session.Haptics().Motors(HapticFeedback::Clock::now(), false);
		}
		const bool enabled = m_session.Connected() && m_session.Debugger().Selected().running && m_window->Focused() &&
							 m_window->HasGamepad();
		const auto motors = m_session.Haptics().Motors(HapticFeedback::Clock::now(), enabled);
		if (!m_window->Rumble(motors, m_session.Haptics().Deadline()) && !m_rumbleFailed)
		{
			m_rumbleFailed = true;
			m_session.Notify("Gamepad rumble unavailable", "This gamepad could not accept vibration output.");
		}
	}
	m_range.Poll();
}
RuntimeView RuntimePanels::Build() const
{
	RuntimeView view{m_session.View()};
	const auto recording = m_recording.Snapshot();
	view.recording = recording.page && recording.page->active;
	view.recordingOverflow = recording.page && recording.page->overflow;
	view.recordingBusy = recording.busy;
	view.recordedChanges = recording.page ? recording.page->total : 0;
	view.operationPending |= m_replayPreparation.has_value();
	view.rigExpanded = m_frame.Active() ? m_frame.Expanded() : m_spatial.Expanded();
	if (m_frame.Active())
	{
		view.rigView = m_frame.RigViewRect();
		view.rigDevices = m_frame.RigDevicePoints();
	}
	else
	{
		view.rotationBall = m_spatial.RotationBallRect();
		view.markers = m_spatial.MarkerRects();
		view.rigView = m_spatial.RigViewRect();
		view.rigDevices = m_spatial.RigDevicePoints();
	}
	if (view.notice.empty())
		view.notice = m_lastNotice;
	return view;
}
bool RuntimePanels::CanRecord() const
{
	return m_session.Connected() && m_session.Debugger().Selected().id &&
		   m_session.Supports(wire::Capability::CommandRecording);
}
const RuntimeView& RuntimePanels::BeginFrame()
{
	const auto connection = m_session.Connection();
	const auto selected = m_session.Debugger().Selected();
	const bool connected = m_session.Connected();
	m_recording.SetTarget(connected && selected.id ? RecordingTarget{connection.path, selected.id, selected.generation}
												   : RecordingTarget{});
	m_recording.Poll(CanRecord());
	const auto recordingError = m_recording.Snapshot().error;
	if (recordingError != m_recordingError)
	{
		m_recordingError = recordingError;
		if (!recordingError.empty())
			m_session.Notify("Recording failed", recordingError);
	}
	m_view = Build();
	const bool following = m_view.time.following;
	const auto image = m_session.Preview().Selected();
	const auto historical = m_session.Workspace().Selected();
	m_spatial.SetContext(
		connected && selected.id && following && !m_replayPreparation && !m_pilot.Active() &&
			(!m_window || m_window->Focused()),
		!following, image ? std::span<const FrameView>{image->description.views} : std::span<const FrameView>{},
		historical && !following ? &historical->document : nullptr, historical ? historical->local : 0);
	m_session.Preview().BeginFrame();
	return m_view;
}
void RuntimePanels::UndoChanges(std::size_t count)
{
	if (m_replayPreparation || m_pilot.Active() || m_spatial.Active() || m_devices.Active() || !m_view.time.following ||
		!m_session.Setup())
		return;
	if (m_undo.Empty() || count == 0)
		return;
	auto history = m_undo;
	if (auto undo = history.Undo(*m_session.Setup(), m_session.Input().source, count))
	{
		if (m_session.SubmitEdit(*undo))
			m_undo = std::move(history);
	}
	else
		m_session.Notify("Undo unavailable", "Release the affected devices or restore their tracking first");
}
void RuntimePanels::DrawPreview(ShellState& shell)
{
	m_frame.Prepare(shell.selection, m_view);
	const auto& io = ImGui::GetIO();
	if (io.KeyCtrl && !io.KeyShift && ImGui::IsKeyPressed(ImGuiKey_Z, false) && !io.WantTextInput &&
		!ImGui::IsAnyItemActive() &&
		!ImGui::IsPopupOpen(nullptr, ImGuiPopupFlags_AnyPopupId | ImGuiPopupFlags_AnyPopupLevel))
		UndoChanges(1);
	if (!m_preview.Draw(shell, m_view, !m_replayPreparation, !m_replayActive))
		StopPilot();
}
void RuntimePanels::DrawDevicePage(ShellState& shell)
{
	if (!m_view.connected)
	{
		// The page keeps its title, greyed; the preview's Open app is the one way in.
		ImGui::BeginDisabled();
		m_devices.DrawHeader(shell.selection, false);
		ImGui::EndDisabled();
		ImGui::TextDisabled("Values appear once an app connects");
		return;
	}
	ImGui::BeginDisabled(m_replayPreparation.has_value() || !m_view.time.following || !m_session.SupportsEdits());
	if (auto edit = m_devices.DrawHeader(shell.selection, m_view.time.following))
		SubmitEdit(*edit);
	ImGui::EndDisabled();
	m_spatial.DrawRigView(shell.selection);
	m_devices.SetRigPiloting(m_pilot.Active() || m_spatial.Active() || m_devices.Active());
	if (!m_view.time.following)
	{
		// Looking back: the rig above shows the recorded poses; live controls stay hidden.
		ImGui::TextColored(theme::Vec(theme::Past), "PAST");
		ImGui::SameLine();
		ImGui::TextDisabled("Frame %llu", static_cast<unsigned long long>(m_view.time.cursor.frame));
		ImGui::SameLine();
		if (ImGui::SmallButton("Frame details"))
			shell.selection = FrameSel{m_view.time.cursor, m_view.time.epoch};
		DrawPastDevice(shell.selection);
		return;
	}
	const auto placement = [&]
	{
		m_spatial.DrawPlacement(shell.selection);
	};
	if (auto edit = m_devices.Draw(!m_replayPreparation && m_session.SupportsEdits(), &shell.selection, placement))
		SubmitEdit(*edit);
	if (const auto* device = std::get_if<DeviceSel>(&shell.selection);
		device && (device->page == DevicePage::Left || device->page == DevicePage::Right))
		DrawHapticEvents(m_session.Haptics(), device->page == DevicePage::Left ? 0 : 1,
						 m_session.Connected() && m_session.Supports(wire::Capability::LiveHaptics));
	if (auto device = std::get_if<DeviceSel>(&shell.selection); device && device->page == DevicePage::Trackers)
		device->slot = static_cast<unsigned>(m_devices.SelectedTracker());
}
void RuntimePanels::DrawPastDevice(const Selection& selection)
{
	const auto* device = std::get_if<DeviceSel>(&selection);
	if (!device ||
		(device->page != DevicePage::Body && device->page != DevicePage::Gaze && device->page != DevicePage::Trackers))
		return;
	const auto& history = m_session.History();
	const auto index = history.Find(m_view.time.cursor, m_view.time.epoch);
	const auto* sample = index ? m_session.InputSamples().Find(history.At(*index).input) : nullptr;
	if (!sample)
	{
		ImGui::TextDisabled("Input not kept for this frame");
		return;
	}
	if (device->page == DevicePage::Body)
		DrawPastBody(*sample);
	else if (device->page == DevicePage::Gaze)
		DrawPastGaze(*sample);
	else
		DrawPastTrackers(*sample, device->slot);
}
std::vector<Notice> RuntimePanels::TakeNotices()
{
	auto notices = m_session.TakeNotices();
	if (!notices.empty())
		m_lastNotice = notices.back().title + (notices.back().detail.empty() ? "" : ": " + notices.back().detail);
	return notices;
}
void RuntimePanels::SubmitEdit(const SimulationEdit& edit)
{
	StopPilot();
	if (m_session.SubmitEdit(edit))
		if (const auto transaction = std::get_if<ManualTransaction>(&edit); transaction && m_session.Setup())
			m_undo.Record(*m_session.Setup(), *transaction);
}
void RuntimePanels::StopPilot()
{
	m_pilot.Stop(m_window, m_session.ManualInput(), true);
	m_devices.SetRigPiloting(false);
}
void RuntimePanels::FocusLost()
{
	m_preview.CancelInteraction();
	m_spatial.Cancel();
	m_undo.FinishGesture();
	m_session.Haptics().Motors(HapticFeedback::Clock::now(), false);
	if (m_window)
		m_window->Rumble({});
	m_pilot.Stop(m_window, m_session.ManualInput(), true, true);
	m_devices.SetRigPiloting(false);
	m_devices.ClearHeldInputs();
}
void RuntimePanels::CancelPreviewInteraction()
{
	m_preview.CancelInteraction();
}
bool RuntimePanels::ReleaseInputs()
{
	StopPilot();
	m_devices.ClearHeldInputs();
	if (m_session.Connected() && m_session.Debugger().Selected().id && m_session.Debugger().Selected().running)
		return m_session.SubmitEdit(ReleaseManualInputs{});
	return false;
}
bool RuntimePanels::RequestExport(std::string path)
{
	if (m_range.Exporting())
	{
		m_session.Notify("Export busy", "Wait for the range export to finish");
		return false;
	}
	return m_session.RequestExport(std::move(path));
}
const ImageExportTask& RuntimePanels::ImageExport() const noexcept
{
	return m_session.Exports();
}
std::optional<automation::SavedRecording> RuntimePanels::TakeRecordingReplay()
{
	if (auto play = m_range.TakePlayRequest())
		m_recordingReplay = RecordingReplay{std::move(*play), m_session.Debugger().Scope()};
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
RuntimePanels::RuntimePanels(UiRenderer& renderer, std::string endpoint, Window* window)
	: m_session(renderer, endpoint), m_window(window), m_connection(endpoint)
{
}
RuntimePanels::~RuntimePanels()
{
	m_spatial.Cancel();
	StopPilot();
	if (m_window)
		m_window->Rumble({});
}
void RuntimePanels::ConfigurePreviews(std::uint32_t budgetMiB)
{
	m_session.ConfigurePreviews(budgetMiB);
}
void RuntimePanels::ConfigureSnapshots(std::uint32_t budgetMiB)
{
	if (!m_replayPreparation && !m_replayActive)
		m_session.ConfigureSnapshots(budgetMiB);
}
void RuntimePanels::ConfigurePilot(const PilotBindings& keys, const GamepadBindings& gamepad, bool remoteMouseDrag)
{
	m_pilot.Configure(keys, gamepad, remoteMouseDrag);
}
const RuntimeView& RuntimePanels::View() const
{
	return m_view;
}
bool RuntimePanels::PilotActive() const
{
	return m_pilot.Active();
}
bool& RuntimePanels::HudFolded()
{
	return m_pilot.HudFolded();
}
std::optional<ImVec4>& RuntimePanels::PreviewCrop()
{
	return m_preview.Crop();
}
void RuntimePanels::DrawFramePage(ShellState& shell)
{
	ImGui::BeginDisabled(m_replayPreparation.has_value());
	m_frame.Draw(shell, m_view);
	ImGui::EndDisabled();
}
void RuntimePanels::DrawRangePage(const RangeSel& range, ShellState& shell)
{
	ImGui::BeginDisabled(m_replayPreparation.has_value());
	if (auto replay = m_range.Draw(range, shell))
		m_recordingReplay = RecordingReplay{std::move(*replay), m_session.Debugger().Scope()};
	ImGui::EndDisabled();
}
void RuntimePanels::DrawAdvancedPage()
{
	ImGui::BeginDisabled(m_replayPreparation.has_value());
	m_advanced.Draw();
	ImGui::EndDisabled();
}
void RuntimePanels::DrawSessionMenu()
{
	ImGui::BeginDisabled(m_replayPreparation.has_value() || !m_view.connected || !m_view.time.following ||
						 !m_session.SupportsEdits());
	if (auto edit = m_devices.DrawSessionMenu())
		SubmitEdit(*edit);
	ImGui::EndDisabled();
}
void RuntimePanels::DrawRecordingPopovers()
{
	m_recording.DrawPopovers(CanRecord());
	if (auto saved = m_recording.TakeSaved())
		m_savedTake = std::move(*saved);
}
std::optional<std::string> RuntimePanels::TakeSavedTake()
{
	return std::exchange(m_savedTake, {});
}
bool RuntimePanels::CanReplay() const
{
#ifdef SIMXR_COMPANION_AUTOMATION
	return CanRecord() && !m_replayActive && !m_replayPreparation;
#else
	return false;
#endif
}
void RuntimePanels::RequestReplay(automation::SavedRecording recording)
{
	m_recordingReplay = RecordingReplay{std::move(recording), m_session.Debugger().Scope()};
}
void RuntimePanels::DrawConnection(bool* open)
{
	m_connection.Draw(m_session, open);
}
void RuntimePanels::DrawWindows()
{
	m_documents.DrawWindow();
}
void RuntimePanels::EndFrame()
{
	m_spatial.EndFrame();
	if (auto release = m_devices.EndInputFrame())
		m_session.SubmitEdit(*release);
	if (!m_spatial.Active() && !ImGui::IsAnyItemActive())
		m_undo.FinishGesture();
}
std::uint64_t RuntimePanels::PreviewBytes() const
{
	return m_session.PreviewBytes();
}
std::uint64_t RuntimePanels::PreviewBudget() const
{
	return m_session.PreviewBudget();
}
PreviewHistory& RuntimePanels::KeptPreviews()
{
	return m_session.Preview().History();
}
const PreviewHistory& RuntimePanels::KeptPreviews() const
{
	return m_session.Preview().History();
}
void RuntimePanels::RecordTransfers(VkCommandBuffer command)
{
	m_session.Preview().RecordTransfers(command);
}
std::vector<VkSemaphore> RuntimePanels::ReadyWaits() const
{
	return m_session.Preview().ReadyWaits();
}
void RuntimePanels::Submitted(std::uint64_t serial)
{
	m_session.Preview().Submitted(serial);
}
RuntimeView RuntimePanels::Snapshot() const
{
	return Build();
}
void RuntimePanels::Command(control::CommandKind kind, std::uint64_t capture, bool raw, std::uint32_t layer,
							std::uint32_t view)
{
	if (!m_replayPreparation)
		m_session.Command(kind, capture, raw, layer, view);
}
void RuntimePanels::Pause()
{
	if (!m_replayPreparation)
		m_session.Pause();
}
void RuntimePanels::Step()
{
	if (!m_replayPreparation)
		m_session.Step();
}
std::optional<SteppedFrame> RuntimePanels::TakeSteppedFrame()
{
	return m_session.TakeSteppedFrame();
}
void RuntimePanels::TogglePin(std::size_t index)
{
	if (!m_replayPreparation)
		m_session.TogglePin(index);
}
void RuntimePanels::MarkHistoryFrame(std::size_t index, bool enabled)
{
	m_session.MarkHistoryFrame(index, enabled);
}
void RuntimePanels::LookAt(std::size_t index)
{
	m_session.LookAt(index);
}
const HistoryStore& RuntimePanels::History() const
{
	return m_session.History();
}
const InputHistory& RuntimePanels::InputSamples() const
{
	return m_session.InputSamples();
}
std::vector<RuntimeFrame> RuntimePanels::TimelineFrames() const
{
	return m_session.TimelineFrames();
}
void RuntimePanels::SelectFrame(std::uint64_t local)
{
	m_session.SelectFrame(local);
}
void RuntimePanels::SelectCapture(std::uint64_t capture)
{
	m_session.SelectCapture(capture);
}
void RuntimePanels::ReleaseSelected()
{
	if (!m_replayPreparation)
		m_session.ReleaseSelected();
}
void RuntimePanels::CancelPending()
{
	if (!m_replayPreparation)
		m_session.CancelPending();
}
std::vector<UndoStack::Change> RuntimePanels::Changes() const
{
	return m_undo.Changes();
}
std::string RuntimePanels::Endpoint() const
{
	return m_session.Endpoint();
}
const std::vector<std::string>& RuntimePanels::Endpoints() const
{
	return m_session.Endpoints();
}
void RuntimePanels::RefreshEndpoints()
{
	m_session.RefreshEndpoints();
}
void RuntimePanels::ConnectEndpoint(std::string endpoint)
{
	m_session.ConnectEndpoint(std::move(endpoint));
}
void RuntimePanels::Disconnect()
{
	m_session.Disconnect();
}
void RuntimePanels::StartRecording()
{
	m_recording.RequestStart();
}
void RuntimePanels::StopRecording()
{
	m_recording.RequestStop();
}
void RuntimePanels::SetReplayActive(bool active)
{
	m_replayActive = active;
}
void RuntimePanels::ResumeLive()
{
	if (!m_replayPreparation)
		m_session.ResumeLive();
}
void RuntimePanels::SetLive(bool enabled)
{
	m_session.SetLive(enabled);
}
std::vector<std::uint64_t> RuntimePanels::CaptureIds() const
{
	return m_session.Workspace().CaptureIds();
}
void RuntimePanels::InspectSelected()
{
	m_session.Workspace().InspectSelected();
}
void RuntimePanels::PrepareReplay()
{
	m_spatial.SetContext(false, false, {}, nullptr, 0);
	m_undo.FinishGesture();
	const auto observation = m_session.ManualInput().Snapshot().observation;
	m_replayPreparation = ReplayPreparation{m_session.Debugger().Scope(), observation};
	m_replayPreparation->releaseQueued = ReleaseInputs();
	m_session.Debugger().Relinquish();
}
void RuntimePanels::FinishReplayPreparation()
{
	m_replayPreparation.reset();
}
bool RuntimePanels::ReplayReady() const
{
	const auto& debugger = m_session.Debugger();
	const auto& setup = m_session.Setup();
	const auto& input = m_session.Input();
	return m_replayPreparation && m_replayPreparation->releaseQueued &&
		   m_replayPreparation->scope == debugger.Scope() && m_session.Connected() && debugger.Selected().running &&
		   input.observation > m_replayPreparation->observation && !input.editPending && !input.posePending &&
		   input.error.empty() && setup && !debugger.Pending() && !debugger.HasLease() && !debugger.Relinquishing() &&
		   !std::ranges::any_of(setup->owners,
								[](const std::optional<CommandSource>& owner)
								{
									return owner && owner->kind == CommandSourceKind::Manual;
								});
}
} // namespace simxr::app
