#include "app/RuntimePanels.h"
#include "app/FrameHistoryFeed.h"
#include "app/DebuggerPanelController.h"
#include "app/DeviceControlPanel.h"
#include "app/ImageExportTask.h"
#include "app/InspectionWorkspace.h"
#include "app/ManualInputController.h"
#include "app/PilotController.h"
#include "app/PreviewImages.h"
#include "app/RecordingPanel.h"
#include "app/RuntimeConnection.h"
#include "app/RuntimeEndpoints.h"
#include "app/SpatialEditor.h"
#include "app/UiRenderer.h"
#include "app/Window.h"
#include "control/ImageInspection.h"
#include "control/InspectionDocument.h"
#include "control/SimulationControl.h"
#include "graphics/SnapshotBudget.h"
#include <imgui_internal.h>

#include <algorithm>
#include <array>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <future>
#include <sstream>
#include <imgui.h>
#include <optional>
#include <stdexcept>
#include <utility>

namespace simxr::app
{
namespace
{
namespace wire = control;
namespace tag = wire::tag;
using Clock = std::chrono::steady_clock;
using Fields = std::vector<wire::Field>;
using panel::Boolean;
using panel::Nested;
using panel::Text;
using panel::U32;
using panel::U64;
std::vector<std::byte> InspectionMember(std::span<const std::byte> bytes, std::string_view name)
{
	wire::Message node;
	node.fields = wire::DecodeFields(bytes).fields;
	const auto data = node.Find(3);
	if (!data)
		return {};
	for (const auto& field : wire::DecodeFields(data->value).fields)
	{
		wire::Message child;
		child.fields = wire::DecodeFields(field.value).fields;
		if (Text(child, 1) == name)
			return field.value;
	}
	return {};
}

wire::Field InspectionScalar(std::span<const std::byte> bytes, std::string_view member)
{
	wire::Message node;
	node.fields = wire::DecodeFields(InspectionMember(bytes, member)).fields;
	return node.Find(3) ? *node.Find(3) : wire::Field{};
}
std::vector<std::vector<std::byte>> InspectionElements(std::span<const std::byte> bytes)
{
	wire::Message node;
	node.fields = wire::DecodeFields(bytes).fields;
	std::vector<std::vector<std::byte>> elements;
	if (const auto data = node.Find(3))
		for (const auto& page : wire::DecodeFields(data->value).fields)
			for (const auto& item : wire::DecodeFields(page.value).fields)
				elements.push_back(item.value);
	return elements;
}

std::string ImageLabel(const PreviewImageSummary& image)
{
	if (image.target == wire::TargetKind::ComposedEyes)
		return "Composited output";
	if (image.target == wire::TargetKind::VirtualDisplay)
		return "Display output";
	if (image.target == wire::TargetKind::LayerView)
		return "Submitted layer " + std::to_string(image.layer + 1) + " / view " + std::to_string(image.view + 1);
	return "Captured image";
}

const char* ConnectionName(RuntimeConnectionState state)
{
	switch (state)
	{
	case RuntimeConnectionState::Disconnected:
		return "Disconnected";
	case RuntimeConnectionState::Connecting:
		return "Connecting";
	case RuntimeConnectionState::Connected:
		return "Connected";
	case RuntimeConnectionState::Failed:
		return "Connection failed";
	}
	return "Unknown";
}
} // namespace
class RuntimePanels::Impl
{
  public:
	Impl(UiRenderer& renderer, const std::string& path, Window* window)
		: m_debugger(m_requests), m_workspace(m_requests), m_preview(renderer, m_requests), m_exports(m_requests),
		  m_endpoints(DiscoverRuntimeEndpoints()), m_window(window)
	{
		std::snprintf(m_endpoint.data(), m_endpoint.size(), "%s", path.c_str());
		std::snprintf(m_exportFile.data(), m_exportFile.size(), "%s", "snapshot.ppm");
		if (!path.empty())
			m_requests.Connect(path);
	}
	~Impl()
	{
		StopPilot(true);
		m_requests.Close();
	}
	void ConfigurePilot(const PilotBindings& keys, const GamepadBindings& gamepad)
	{
		m_pilot.Configure(keys, gamepad);
	}
	void ChooseSession(PanelSession session)
	{
		m_debugger.ChooseSession(session);
		SelectionChanged();
	}
	void Select(std::uint64_t capture)
	{
		m_historySelection.reset();
		m_focusPreview = true;
		const bool canFetch = m_workspace.Select(capture);
		if (canFetch)
		{
			const auto selected = m_workspace.Selected();
			if (selected && !selected->document.Find(tag::Document) && !m_requests.Has(Work::Inspect, capture))
				m_workspace.InspectSelected();
		}
		// Releasing the remote capture does not invalidate an already-owned GPU
		// copy. Select that private image before considering another source read.
		if (m_preview.SelectCapture(m_debugger.Scope(), capture) || !canFetch)
			return;
		auto fields = m_debugger.SessionFields();
		fields.push_back(wire::Field::U64(tag::CaptureId, capture, true));
		m_requests.Send(wire::OpCode::Capture, Work::Capture, std::move(fields), capture);
	}
	void SelectHistoryFrame(std::size_t index)
	{
		const auto frame = m_history.History().At(index);
		bool found = false;
		for (const auto& capture : m_workspace.Captures())
			if (capture.scope.epoch == frame.epoch && capture.scope.session == frame.identity.session &&
				capture.scope.generation == frame.identity.run && capture.frame == frame.identity.frame)
			{
				SelectFrame(capture.local);
				found = true;
				break;
			}
		if (!found)
		{
			m_preview.ClearSelection();
			m_workspace.SelectImage({}, 0);
			for (const auto& image : m_preview.Images())
				if (image.ready && !image.released && image.scope.epoch == frame.epoch &&
					image.scope.session == frame.identity.session && image.scope.generation == frame.identity.run &&
					image.frame == frame.identity.frame)
				{
					m_preview.SelectImage(image.id);
					break;
				}
		}
		m_preview.Follow(false);
		m_following = false;
		m_historySelection = frame;
	}
	const HistoryStore& History() const { return m_history.History(); }
	void MarkHistoryFrame(std::size_t index, bool enabled)
	{
		m_history.History().SetFlags(index, HistoryMarker, enabled);
	}
	void TogglePin(std::size_t index)
	{
		const auto& frame = m_history.History().At(index);
		for (const auto& capture : m_workspace.Captures())
			if (!capture.released && capture.scope == m_debugger.Scope() && capture.scope.epoch == frame.epoch &&
				capture.scope.session == frame.identity.session && capture.scope.generation == frame.identity.run &&
				capture.frame == frame.identity.frame)
			{
				const bool pinned = Boolean(capture.metadata, tag::Pinned);
				m_debugger.Command(pinned ? wire::CommandKind::UnpinSnapshot : wire::CommandKind::PinSnapshot,
								   capture.id);
				return;
			}
	}
	void UpdateCoverage()
	{
		auto& history = m_history.History();
		for (const auto& frame : m_coverage)
			if (const auto index = history.Find(frame.identity, frame.epoch))
			{
				history.SetCoverage(*index, 0, 0);
				history.SetFlags(*index, HistoryPinned, false);
			}
		m_coverage.clear();
		for (const auto& image : m_preview.Images())
			if (image.ready && !image.released)
				if (const auto index = history.Find({image.scope.session, image.scope.generation, image.frame}, image.scope.epoch))
				{
					const auto frame = history.At(*index);
					history.SetCoverage(*index, image.capture ? image.capture : frame.capture,
						frame.flags | (image.live ? HistoryPreview : HistorySnapshot));
					m_coverage.push_back(frame);
				}
		for (const auto& capture : m_workspace.Captures())
			if (!capture.released && Boolean(capture.metadata, tag::Pinned))
				if (const auto index = history.Find({capture.scope.session, capture.scope.generation, capture.frame},
													capture.scope.epoch))
				{
					const auto frame = history.At(*index);
					history.SetFlags(*index, HistoryPinned, true);
					m_coverage.push_back(frame);
				}
	}
	std::vector<RuntimeFrame> TimelineFrames() const
	{
		std::vector<RuntimeFrame> frames;
		const auto images = m_preview.Images();
		const auto selected = m_workspace.Selected();
		for (const auto& capture : m_workspace.Captures())
		{
			if (!capture.frame)
				continue;
			const bool imageReady =
				std::any_of(images.begin(), images.end(),
							[&](const auto& image)
							{
								return image.scope == capture.scope && image.capture == capture.id &&
									   image.frame == capture.frame && image.ready && !image.released;
							});
			frames.push_back({{capture.scope.epoch, capture.scope.session, capture.scope.generation},
							  capture.local,
							  capture.id,
							  capture.frame,
							  !m_preview.Following() && selected && selected->local == capture.local,
							  imageReady,
							  capture.document.Find(tag::Document) != nullptr,
							  !capture.released && capture.scope == m_debugger.Scope() &&
								  m_requests.Snapshot().state == RuntimeConnectionState::Connected});
		}
		return frames;
	}
	void SelectFrame(std::uint64_t local)
	{
		m_historySelection.reset();
		m_following = false;
		for (const auto& capture : m_workspace.Captures())
			if (capture.local == local)
			{
				if (capture.scope == m_debugger.Scope())
					Select(capture.id);
				else
				{
					m_workspace.SelectLocal(local);
					if (!m_preview.SelectCapture(capture.scope, capture.id))
						m_preview.ClearSelection();
					m_preview.Follow(false);
					m_focusPreview = true;
				}
				return;
			}
	}

	void Command(wire::CommandKind kind, std::uint64_t capture = 0, bool raw = false, std::uint32_t layer = 0,
				 std::uint32_t view = 0)
	{
		if (kind == wire::CommandKind::Pause || kind == wire::CommandKind::Step)
			m_followOnResume = false;
		m_debugger.Command(kind, capture, raw, layer, view);
	}
	void Pause()
	{
		Command(wire::CommandKind::Pause);
	}
	void Step()
	{
		m_following = true;
		m_stepRequested = true;
		m_historySelection.reset();
		Command(wire::CommandKind::Step);
	}
	std::optional<SteppedFrame> TakeSteppedFrame()
	{
		return std::exchange(m_stepped, {});
	}
	void ResumeLive()
	{
		m_following = true;
		m_historySelection.reset();
		m_resumeAfter = m_debugger.Operations().empty() ? 0 : m_debugger.Operations().back().local;
		m_followOnResume = true;
		m_debugger.Command(wire::CommandKind::Resume);
	}

	void StartRecording()
	{
		m_recording.RequestStart();
	}
	void StopRecording()
	{
		m_recording.Stop();
	}
	void SetReplayActive(bool active)
	{
		m_replayActive = active;
	}

	void CancelPending()
	{
		m_debugger.CancelPending();
	}
	void FocusLost()
	{
		m_pilot.Stop(m_window, m_manualInput, true, true);
		m_devices.SetRigPiloting(false);
		m_devices.ClearHeldInputs();
	}
	void ReleaseInputs()
	{
		StopPilot(true);
		m_devices.ClearHeldInputs();
		if (m_debugger.Selected().id && m_debugger.Selected().running)
			SubmitEdit(ReleaseManualInputs{});
	}
	void ReleaseSelected()
	{
		if (const auto capture = m_workspace.Selected();
			capture && !capture->released && capture->scope == m_debugger.Scope())
			m_debugger.Command(wire::CommandKind::Release, capture->id);
		m_workspace.ReleaseSelected();
		m_preview.ReleaseSelected();
	}
	void InspectSelected()
	{
		m_workspace.InspectSelected();
	}
	void RequestExport(std::string path)
	{
		const auto image = m_preview.Selected();
		if (!m_rangeImages.empty() || !image || !image->reader || image->scope.epoch != m_requests.Epoch() ||
			!m_exports.Request(image->reader, std::move(path)))
			m_detail = "Retain an exact source image before requesting export";
	}
	void ConnectEndpoint(std::string path)
	{
		if (path.size() >= m_endpoint.size())
			throw std::invalid_argument("Runtime endpoint is too long");
		std::snprintf(m_endpoint.data(), m_endpoint.size(), "%s", path.c_str());
		m_requests.Connect(std::move(path));
	}
	std::string Endpoint() const
	{
		return m_endpoint.data();
	}
	void SetLive(bool enabled)
	{
		m_following = enabled;
		if (enabled)
			m_historySelection.reset();
		m_preview.Follow(enabled);
	}
	std::vector<std::uint64_t> CaptureIds() const
	{
		return m_workspace.CaptureIds();
	}
	std::vector<VkSemaphore> ReadyWaits() const
	{
		return m_preview.ReadyWaits();
	}
	void Submitted(std::uint64_t serial)
	{
		m_preview.Submitted(serial);
	}
	RuntimePanelSnapshot Snapshot() const
	{
		RuntimePanelSnapshot snapshot;
		if (m_history.History().Size())
			snapshot.historySeconds = m_history.History().At(m_history.History().Size()-1).seconds;
		const auto connection = m_requests.Snapshot();
		const auto selected = m_debugger.Selected();
		snapshot.connected = connection.state == RuntimeConnectionState::Connected;
		snapshot.connectionState = ConnectionName(connection.state);
		snapshot.endpoint = connection.path;
		snapshot.sessionAvailable = selected.id != 0;
		snapshot.epoch = m_requests.Epoch();
		snapshot.latestFrame = U64(Nested(m_debugger.Status().Find(tag::Presentation)), tag::LastSubmitted);
		snapshot.session = selected.id;
		snapshot.generation = selected.generation;
		snapshot.manual = m_debugger.Manual();
		const auto retention = Nested(m_debugger.Status().Find(tag::Retention));
		const auto capacity = U64(retention, tag::Capacity);
		snapshot.historyFull = capacity && U64(retention, tag::Pinned) >= capacity;
		snapshot.following = m_following;
		const auto recording = m_recording.Snapshot();
		snapshot.recording = recording.page && recording.page->active;
		snapshot.recordingOverflow = recording.page && recording.page->overflow;
		snapshot.recordingBusy = recording.busy;
		snapshot.recordedChanges = recording.page ? recording.page->total : 0;
		snapshot.selectedCapture = m_workspace.SelectedId();
		if (const auto capture = m_workspace.Selected())
			snapshot.selectedPinned = Boolean(capture->metadata, tag::Pinned);
		snapshot.captures = m_workspace.Captures().size();
		snapshot.images = m_preview.ReadyCount();
		snapshot.appliedPilotUpdates = m_appliedPilotUpdates;
		snapshot.pending = m_requests.Pending() || m_exports.Busy();
		snapshot.error = m_detail.empty() ? connection.detail : m_detail;
		snapshot.operationPending = m_debugger.Pending();
		if (!m_debugger.Operations().empty())
		{
			const auto& operation = m_debugger.Operations().back();
			snapshot.lastOperation = operation.id;
			snapshot.phase = operation.phase;
			snapshot.progress = operation.progress;
		}
		if (!m_workspace.Captures().empty())
			snapshot.lastCapture = m_workspace.Captures().back().id;
		if (const auto image = m_preview.Selected())
		{
			snapshot.selectedEpoch = image->scope.epoch;
			snapshot.selectedIdentity = {image->scope.session, image->scope.generation, image->frame};
			snapshot.selectedFrame = image->frame;
			snapshot.selectedImageCapture = image->capture;
			snapshot.selectedReader = image->reader;
			snapshot.selectedReady = image->ready;
			snapshot.selectedLayers = image->description.arrayLayers;
			snapshot.selectedViews = image->description.views;
		}
		if (const auto capture = m_workspace.Selected(); capture && capture->document.Find(tag::Document))
		{
			snapshot.inspectedFrame = panel::Frame(capture->document);
			if (!snapshot.selectedFrame && !snapshot.following)
			{
				snapshot.selectedEpoch = capture->scope.epoch;
				snapshot.selectedFrame = capture->frame;
				snapshot.selectedIdentity = {capture->scope.session, capture->scope.generation, capture->frame};
			}
		}
		if (!m_preview.Following() && m_historySelection)
		{
			snapshot.selectedEpoch = m_historySelection->epoch;
			snapshot.selectedIdentity = m_historySelection->identity;
			snapshot.selectedFrame = m_historySelection->identity.frame;
		}
		snapshot.rigExpanded = m_spatial.Expanded();
		snapshot.rotationBall = m_spatial.RotationBallRect();
		snapshot.reachMap = m_spatial.ReachMapRect();
		snapshot.markers = m_spatial.MarkerRects();
		return snapshot;
	}
	std::optional<automation::SavedRecording> TakeRecordingReplay()
	{
		auto replay = std::exchange(m_recordingReplay, {});
		if (!replay)
			return {};
		if (replay->scope != m_debugger.Scope())
		{
			m_detail = "Recording replay target changed; select the intended session and replay again";
			return {};
		}
		return std::move(replay->saved);
	}
	void Poll()
	{
		if (m_requests.RefreshEpoch())
		{
			m_debugger.ResetEpoch();
			SelectionChanged();
			m_exports.ResetEpoch();
			m_nextPreview = {};
		}
		for (const auto& reply : m_requests.Poll())
			try
			{
				Reply(reply);
			}
			catch (const std::exception& error)
			{
				m_detail = error.what();
			}
		if (m_requests.Snapshot().state == RuntimeConnectionState::Connected)
		{
			m_requests.FlushReleases();
			m_debugger.Poll();
			const auto selected = m_debugger.Selected();
			const auto now = Clock::now();
			m_history.SetScope(m_debugger.Scope());
			if (selected.id && now >= m_nextHistory && !m_requests.Has(Work::FrameHistory) &&
				(U64(m_debugger.Status(), tag::Capabilities) & static_cast<std::uint64_t>(wire::Capability::FrameHistory)))
			{
				auto fields = m_debugger.SessionFields();
				fields.push_back(wire::Field::U64(tag::Offset, m_history.After()));
				m_requests.Send(wire::OpCode::ReadFrameHistory, Work::FrameHistory, std::move(fields));
				m_nextHistory = now + std::chrono::milliseconds(100);
			}
			m_manualInput.SetTarget(
				selected.id && (U64(m_debugger.Status(), tag::Capabilities) &
								static_cast<std::uint64_t>(wire::Capability::SimulationCommands))
					? ManualInputTarget{m_requests.Snapshot().path, selected.id, selected.generation}
					: ManualInputTarget{});
			const auto input = m_manualInput.Snapshot();
			m_spatial.Observe(input, m_pilot.Active());
			m_devices.ObserveDelivery(input.editPending, input.error);
			m_devices.SetManualSource(input.source);
			m_appliedPilotUpdates = input.appliedPoses;
			m_devices.SetRigPiloting(m_pilot.Active() || m_spatial.Active());
			m_simulationState = input.state;
			m_simulationResult = input.result;
			if (m_observation != input.observation)
			{
				m_observation = input.observation;
				m_devices.ObserveState(input.state);
			}
			if (!input.error.empty())
			{
				m_detail = input.error;
				StopPilot(true);
			}
			if (const auto capture = m_workspace.Selected();
				capture && !capture->released && capture->scope == m_debugger.Scope() && now >= m_nextCapture &&
				(capture->phase == "pending" || capture->phase == "reserved" || capture->phase == "issued" ||
				 capture->phase == "begun" || capture->phase == "gpu_pending") &&
				!m_requests.Has(Work::Capture, capture->id))
			{
				auto fields = m_debugger.SessionFields();
				fields.push_back(wire::Field::U64(tag::CaptureId, capture->id, true));
				m_requests.Send(wire::OpCode::Capture, Work::Capture, std::move(fields), capture->id);
				m_nextCapture = now + std::chrono::milliseconds(100);
			}
			if (m_preview.Following() && selected.id && now >= m_nextPreview && !m_requests.Has(Work::Preview) &&
				!m_preview.HasPendingLive() &&
				(U64(m_debugger.Status(), tag::Capabilities) &
				 static_cast<std::uint64_t>(wire::Capability::LivePreview)))
			{
				m_requests.Send(wire::OpCode::RetainPreview, Work::Preview, m_debugger.SessionFields());
				m_nextPreview = now + std::chrono::milliseconds(m_pilot.Active() ? 16 : 33);
			}
		}
		if (m_requests.Snapshot().state != RuntimeConnectionState::Connected)
			m_manualInput.SetTarget({});
		if (m_followOnResume)
			for (const auto& operation : m_debugger.Operations())
				if (operation.local > m_resumeAfter && operation.kind == wire::CommandKind::Resume &&
					panel::Terminal(operation.phase))
				{
					if (operation.phase != "complete")
						m_followOnResume = false;
					else if (!m_debugger.Manual() && m_requests.Snapshot().state == RuntimeConnectionState::Connected)
					{
						m_preview.Follow(true);
						m_followOnResume = false;
					}
				}
		m_exports.Poll();
		PollRangeExports();
		m_preview.Poll();
		UpdateCoverage();
		for (auto error :
			 {m_requests.TakeError(), m_debugger.TakeError(), m_workspace.TakeError(), m_preview.TakeError()})
			if (!error.empty())
				m_detail = std::move(error);
	}
	void Draw(ShellState& shell)
	{
		const bool devicePage = std::holds_alternative<DeviceSel>(shell.selection) ||
								std::holds_alternative<SessionItemSel>(shell.selection);
		const bool framePage = std::holds_alternative<FrameSel>(shell.selection);
		const bool advancedPage = std::holds_alternative<AdvancedSel>(shell.selection);
		const auto selected = m_debugger.Selected();
		const auto& status = m_debugger.Status();
		const auto captures = m_workspace.Captures();
		const bool manual = m_debugger.Manual(), lease = m_debugger.HasLease();
		const auto epoch = m_requests.Epoch();
		const auto selectedRecord = m_workspace.Selected() ? m_workspace.Selected()->local : 0;
		const auto connectionState = m_requests.Snapshot();
		const bool connected = connectionState.state == RuntimeConnectionState::Connected;
		m_recording.SetTarget(connected && selected.id
								  ? RecordingTarget{connectionState.path, selected.id, selected.generation}
								  : RecordingTarget{});
		const bool canRecord =
			connected && selected.id &&
			(U64(status, tag::Capabilities) & static_cast<std::uint64_t>(wire::Capability::CommandRecording));
		m_recording.Poll(canRecord);
#ifdef SIMXR_COMPANION_AUTOMATION
		const bool replayAvailable = connected && selected.id && !m_replayActive;
#else
		constexpr bool replayAvailable = false;
#endif
		const auto spatialImage = m_preview.Selected();
		const auto historical = m_workspace.Selected();
		m_spatial.SetContext(
			connected && selected.id && m_preview.Following() && !m_pilot.Active(), !m_preview.Following(),
			spatialImage ? std::span<const FrameView>{spatialImage->description.views} : std::span<const FrameView>{},
			historical && !m_preview.Following()
				? &historical->document
				: nullptr,
			historical ? historical->local : 0);
		ImGui::SetCursorPos({shell.inspector.x, shell.inspector.y});
		ImGui::BeginChild("Inspector", {shell.inspector.width, shell.inspector.height});
		if (shell.inspectorVisible && std::holds_alternative<LibrarySel>(shell.selection))
			if (auto replay = m_recording.Draw(canRecord, replayAvailable, m_replayActive))
				m_recordingReplay = RecordingReplay{std::move(*replay), m_debugger.Scope()};
		m_preview.BeginFrame();
		const auto state = m_requests.Snapshot();
		if (shell.inspectorVisible && advancedPage)
		{
			ImGui::SeparatorText("Advanced stepping");
			if (auto edit = m_devices.DrawAdvanced())
				SubmitEdit(*edit);
			ImGui::BeginDisabled(!selected.id || state.state != RuntimeConnectionState::Connected);
			if (ImGui::Button("Cancel pending operation"))
				CancelPending();
			if (ImGui::CollapsingHeader("Advanced stepping"))
			{
				if (ImGui::Button("Advance display only"))
					Command(wire::CommandKind::DisplayStep);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Advances presentation without admitting another application frame.");
				if (ImGui::Button("Strict frame admission"))
					Command(wire::CommandKind::StrictAdmission);
				ImGui::SeparatorText("Capture a submitted source");
				ImGui::TextUnformatted("Next submitted source");
				ImGui::TextUnformatted("Submitted layer index");
				ImGui::SetNextItemWidth(-1);
				ImGui::InputInt("##Source layer", &m_rawLayer);
				ImGui::TextUnformatted("Projection view index");
				ImGui::SetNextItemWidth(-1);
				ImGui::InputInt("##Source view", &m_rawView);
				if (ImGui::Button("Advance and capture source"))
					Command(wire::CommandKind::Step, 0, true, static_cast<std::uint32_t>(std::max(0, m_rawLayer)),
							static_cast<std::uint32_t>(std::max(0, m_rawView)));
			}
			ImGui::EndDisabled();
			ImGui::SeparatorText("Runtime progress");
			if (!selected.id)
				ImGui::TextUnformatted("Session unavailable");
			else
				ImGui::TextWrapped("Admission: %s; controller: %s", manual ? "manual" : "automatic",
								   lease ? "owned by this connection" : "observer");
			const auto pipeline = Nested(status.Find(tag::Pipeline));
			if (!selected.id && status.Find(tag::Pipeline))
				ImGui::TextUnformatted("Last observed pipeline:");
			if (status.Find(tag::Pipeline))
				ImGui::TextWrapped("Permits: %u; wait pending: %s", U32(pipeline, tag::Permits),
								   Boolean(pipeline, tag::WaitPending) ? "yes" : "no");
			for (const auto field : {tag::Waited, tag::Begun})
				if (const auto value = pipeline.Find(field))
					ImGui::Text("%s frame: %llu", field == tag::Waited ? "Waited" : "Begun",
								static_cast<unsigned long long>(U64(Nested(value), tag::FrameId)));
			const auto presentation = Nested(status.Find(tag::Presentation));
			const auto display = Nested(presentation.Find(tag::Display));
			if (status.Find(tag::Presentation))
				ImGui::TextWrapped("Last submitted: %llu; display tick: %llu; selection: %s",
								   static_cast<unsigned long long>(U64(presentation, tag::LastSubmitted)),
								   static_cast<unsigned long long>(U64(display, tag::Tick)),
								   Text(display, tag::Kind).c_str());
			for (const auto& operation : m_debugger.Operations())
				ImGui::TextWrapped("Operation %llu: %s / %s", static_cast<unsigned long long>(operation.id),
								   operation.phase.c_str(), operation.progress.c_str());

			ImGui::SeparatorText("Snapshots");
			ImGui::Checkbox("Select completed steps", &m_autoSelect);
			for (const auto& capture : captures)
			{
				const auto label = "Capture " + std::to_string(capture.id) + " / frame " +
								   std::to_string(capture.frame) + " / " + capture.phase +
								   (capture.released ? " (released)" : "");
				ImGui::PushID(static_cast<int>(capture.local));
				if (ImGui::Selectable(label.c_str(), capture.local == selectedRecord))
					SelectFrame(capture.local);
				ImGui::PopID();
			}
			ImGui::SeparatorText("Private GPU images");
			for (const auto& image : m_preview.Images())
			{
				const auto label = "Frame " + std::to_string(image.frame) + " / session " +
								   std::to_string(image.scope.session) + " / run " +
								   std::to_string(image.scope.generation) +
								   (image.live ? " / live" : " / capture " + std::to_string(image.capture));
				ImGui::PushID(static_cast<int>(image.id));
				if (ImGui::Selectable(label.c_str(), image.id == m_preview.SelectedId()))
				{
					m_preview.SelectImage(image.id);
					m_workspace.SelectImage(image.scope, image.capture);
					m_focusPreview = true;
				}
				ImGui::PopID();
			}
			if (const auto capture = m_workspace.Selected();
				capture && !capture->released && capture->scope == m_debugger.Scope() &&
				(U64(m_debugger.Status(), tag::Capabilities) &
				 static_cast<std::uint64_t>(wire::Capability::SnapshotPins)))
			{
				const bool pinned = Boolean(capture->metadata, tag::Pinned);
				if (ImGui::Button(pinned ? "Unpin snapshot" : "Pin snapshot"))
					m_debugger.Command(pinned ? wire::CommandKind::UnpinSnapshot : wire::CommandKind::PinSnapshot,
									   capture->id);
			}
			if (ImGui::Button("Release selected snapshot"))
				ReleaseSelected();
			const auto capture = m_workspace.Selected();
			ImGui::BeginDisabled(!capture || capture->released || capture->scope.epoch != epoch ||
								 capture->scope.session != selected.id ||
								 capture->scope.generation != selected.generation);
			if (ImGui::Button("Refresh frame details"))
				m_workspace.InspectSelected();
			ImGui::EndDisabled();
		}
		if (shell.inspectorVisible)
			if (const auto* range = std::get_if<RangeSel>(&shell.selection))
				DrawRange(*range, shell);
		if (shell.inspectorVisible && framePage)
			DrawFramePage(shell);
		if (shell.inspectorVisible && devicePage)
			DeviceControls(shell.selection);
		ImGui::EndChild();
		if (m_rawOpen)
		{
			ImGui::SetNextWindowSize({600, 480}, ImGuiCond_FirstUseEver);
			if (ImGui::Begin("Raw metadata", &m_rawOpen))
			{
				m_nodesDrawn = 0;
				m_documentEpoch = m_rawEpoch;
				Document(m_rawDocument);
			}
			ImGui::End();
		}
		ImGui::SetCursorPos({shell.preview.x, shell.preview.y});
		bool pilotArea = false;
		m_focusPreview = false;
		if (ImGui::BeginChild("GPU preview", {shell.preview.width, shell.preview.height}))
		{
			const auto image = m_preview.Selected();
			const bool live = m_preview.Following();
			const auto accent = !connected ? ImVec4{0.62f, 0.66f, 0.72f, 1}
								: manual   ? ImVec4{0.96f, 0.69f, 0.32f, 1}
								: !live	   ? ImVec4{0.48f, 0.72f, 0.95f, 1}
										   : ImVec4{0.35f, 0.80f, 0.65f, 1};
			ImGui::TextColored(accent, "%s",
							   !connected ? "DISCONNECTED"
							   : manual	  ? "PAUSED"
							   : !live	  ? "PAST"
										  : "LIVE");
			if (image)
			{
				ImGui::SameLine();
				ImGui::TextDisabled("Frame %llu  /  %s", static_cast<unsigned long long>(image->frame),
									ImageLabel(*image).c_str());
			}
			if (!live && !manual && connected)
			{
				if (ImGui::SmallButton("Show live output"))
					m_preview.Follow(true);
				if (ImGui::IsItemHovered())
					ImGui::SetTooltip("Follows new images. The application is already running.");
			}
			if (!m_exports.Detail().empty())
				ImGui::TextWrapped("%s", m_exports.Detail().c_str());
			if (!m_detail.empty())
			{
				ImGui::SeparatorText("Runtime message");
				ImGui::TextWrapped("%s", m_detail.c_str());
				if (ImGui::SmallButton("Dismiss message"))
					m_detail.clear();
			}
			if (!state.detail.empty())
				ImGui::TextWrapped("%s", state.detail.c_str());
			if (image && image->ready && !image->released)
			{
				if (m_previewView >= static_cast<int>(image->slices))
					m_previewView = 0;
				const bool eyes = image->target == wire::TargetKind::ComposedEyes && image->slices == 2;
				if (eyes)
				{
					for (const auto [label, view] : {std::pair{"Left eye", 0}, {"Right eye", 1}, {"Both", -1}})
					{
						if (m_previewView == view)
							ImGui::PushStyleColor(ImGuiCol_Button, ImGui::GetStyleColorVec4(ImGuiCol_ButtonActive));
						const bool chosen = ImGui::Button(label);
						if (m_previewView == view)
							ImGui::PopStyleColor();
						if (chosen)
							m_previewView = view;
						ImGui::SameLine();
					}
				}
				else if (image->slices > 1)
				{
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 7);
					const auto label = m_previewView < 0 ? "All slices" : "Slice " + std::to_string(m_previewView + 1);
					if (ImGui::BeginCombo("##Image slice", label.c_str()))
					{
						for (int view = -1; view < static_cast<int>(image->slices); ++view)
							if (ImGui::Selectable(view < 0 ? "All slices"
														   : ("Slice " + std::to_string(view + 1)).c_str(),
												  m_previewView == view))
								m_previewView = view;
						ImGui::EndCombo();
					}
					ImGui::SameLine();
				}
				if (ImGui::Button("Fit"))
					m_fitPreview = true;
				ImGui::SameLine();
				if (ImGui::Button("1:1"))
				{
					m_fitPreview = false;
					m_zoom = 1;
				}
				ImGui::SameLine();
				if (ImGui::Button("Zoom..."))
					ImGui::OpenPopup("Image zoom");
				if (ImGui::BeginPopup("Image zoom"))
				{
					ImGui::SetNextItemWidth(ImGui::GetFontSize() * 12);
					if (ImGui::SliderFloat("##Zoom", &m_zoom, 0.05f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
						m_fitPreview = false;
					ImGui::TextUnformatted("Middle-drag pans the image.\n1:1 shows native pixels.");
					ImGui::EndPopup();
				}
				if (live && !manual && connected)
				{
					ImGui::SameLine();
					if (ImGui::Button("Pilot help"))
						ImGui::OpenPopup("Piloting");
					if (ImGui::BeginPopup("Piloting"))
					{
						m_pilot.DrawHelp();
						ImGui::EndPopup();
					}
				}

				if (framePage)
				{
					if (ImGui::CollapsingHeader("Image details and export"))
					{
						const auto& description = image->description;
						ImGui::Text("%u x %u / %u views / format %u", description.width, description.height,
									description.arrayLayers, description.format);
						ImGui::InputText("Image file (.ppm or .rgba)", m_exportFile.data(), m_exportFile.size());
						ImGui::BeginDisabled(!image->reader || image->scope.epoch != epoch || m_exports.Busy());
						if (ImGui::Button("Export image"))
							RequestExport(m_exportFile.data());
						ImGui::EndDisabled();
					}
				}
				ImGui::SameLine();
				m_spatial.DrawSpaceToggle();
				ImGui::BeginChild("Image area", {0, 0}, 0, ImGuiWindowFlags_NoScrollWithMouse);
				if (m_spatial.Expanded())
				{
					const auto minimum = ImGui::GetCursorScreenPos();
					const auto available = ImGui::GetContentRegionAvail();
					m_spatial.DrawExpanded(shell.selection, available);
					const auto after = ImGui::GetCursorScreenPos();
					const float width = available.x * .25f;
					const float height = width * image->description.height / image->description.width;
					ImGui::SetCursorScreenPos(
						{minimum.x + available.x - width - 8, minimum.y + available.y - height - 8});
					m_preview.DrawSelected(width, m_previewView < 0 ? 0 : m_previewView);
					ImGui::SetCursorScreenPos({after.x, after.y - ImGui::GetStyle().ItemSpacing.y});
					ImGui::Dummy({0, 0});
				}
				else
				{

					if (!m_pilot.Active() && !m_spatial.Active() && !m_fitPreview && ImGui::IsWindowHovered() &&
						ImGui::GetIO().MouseWheel)
						m_zoom = std::clamp(m_zoom * std::pow(1.15f, ImGui::GetIO().MouseWheel), .05f, 16.0f);
					if (!m_pilot.Active() && !m_spatial.Active() && ImGui::IsWindowHovered() &&
						ImGui::IsMouseClicked(ImGuiMouseButton_Middle) && !m_fitPreview)
						m_panning = true;
					if (m_pilot.Active() || m_spatial.Active() || !ImGui::IsMouseDown(ImGuiMouseButton_Middle) ||
						m_fitPreview)
						m_panning = false;
					if (m_panning)
					{
						ImGui::SetScrollX(ImGui::GetScrollX() - ImGui::GetIO().MouseDelta.x);
						ImGui::SetScrollY(ImGui::GetScrollY() - ImGui::GetIO().MouseDelta.y);
						ImGui::SetMouseCursor(ImGuiMouseCursor_ResizeAll);
					}
					const auto available = ImGui::GetContentRegionAvail();
					const auto& description = image->description;
					const float slices = m_previewView < 0 ? static_cast<float>(image->slices) : 1;
					float width = description.width * m_zoom;
					if (m_fitPreview)
					{
						width = std::max(
							16.0f, std::min((available.x - 12 * (slices - 1)) / slices,
											std::max(16.0f, available.y) * description.width / description.height));
					}
					const float totalWidth = width * slices + 12 * (slices - 1);
					const float height = width * description.height / description.width;
					if (m_fitPreview)
						m_zoom = width / description.width;
					else if (totalWidth != m_previousImageSize.x || height != m_previousImageSize.y)
					{
						ImGui::SetScrollX(std::max(0.0f, (totalWidth - available.x) / 2));
						ImGui::SetScrollY(std::max(0.0f, (height - available.y) / 2));
					}
					m_previousImageSize = {totalWidth, height};
					ImGui::SetCursorPosX(ImGui::GetCursorPosX() + std::max(0.0f, (available.x - totalWidth) / 2));
					ImGui::SetCursorPosY(ImGui::GetCursorPosY() + std::max(0.0f, (available.y - height) / 2));
					const auto imageMin = ImGui::GetCursorScreenPos();
					const ImVec2 imageMax{imageMin.x + totalWidth, imageMin.y + height};
					m_preview.DrawSelected(width, m_previewView < 0 ? std::nullopt
																	: std::optional<std::size_t>(m_previewView));
					bool overlayHovered = false;
					if (!m_pilot.Active() && (!manual || image->frame == m_spatial.ObservedFrame()))
						for (std::size_t ordinal = 0; ordinal < static_cast<std::size_t>(slices); ++ordinal)
						{
							const auto eye = m_previewView < 0 ? ordinal : static_cast<std::size_t>(m_previewView);
							if (eye < description.views.size())
							{
								const ImVec2 eyeMin{imageMin.x + ordinal * (width + 12), imageMin.y};
								overlayHovered |=
									m_spatial.DrawPreview(shell.selection, eyeMin, {eyeMin.x + width, imageMax.y},
														  description.views[eye], static_cast<unsigned>(eye));
							}
						}
					pilotArea = live && connected && selected.running && !manual;
					if (m_window)
					{
						const bool hudHovered = m_pilot.DrawHud(imageMin, imageMax, m_manualInput);
						m_pilot.Update(*m_window, m_manualInput, pilotArea,
									   !hudHovered && !overlayHovered &&
										   ImGui::IsMouseHoveringRect(imageMin, imageMax));
						m_devices.SetRigPiloting(m_pilot.Active() || m_spatial.Active());
						if (!m_pilot.Error().empty())
							m_detail = m_pilot.Error();
					}
				}
				ImGui::EndChild();
			}
			else
			{
				ImGui::Spacing();
				if (m_spatial.Expanded())
					m_spatial.DrawExpanded(shell.selection, ImGui::GetContentRegionAvail());
				else if (!m_preview.Following() && m_historySelection)
					ImGui::TextDisabled("Frame %llu / No image", static_cast<unsigned long long>(m_historySelection->identity.frame));
				else
					ImGui::TextDisabled("%s", connected ? "Waiting for image" : "No app");
			}
		}
		ImGui::EndChild();
		if (!pilotArea)
			StopPilot(true);
	}

	void DrawConnection(bool* open)
	{
		const auto selected = m_debugger.Selected();
		const auto sessions = m_debugger.Sessions();
		ImGui::SetNextWindowSize({600, 400}, ImGuiCond_FirstUseEver);
		ImGui::OpenPopup("Runtime connection");
		if (!ImGui::BeginPopupModal("Runtime connection", open))
		{
			return;
		}
		const auto state = m_requests.Snapshot();
		ImGui::TextUnformatted(ConnectionName(state.state));
		if (ImGui::Button("Refresh applications"))
			m_endpoints = DiscoverRuntimeEndpoints();
		if (state.state == RuntimeConnectionState::Connected)
		{
			ImGui::SameLine();
			if (ImGui::Button("Disconnect"))
				m_requests.Disconnect();
		}
		if (m_endpoints.empty())
			ImGui::TextUnformatted("No applications");
		for (const auto& path : m_endpoints)
		{
			if (ImGui::Selectable(std::filesystem::path(path).filename().string().c_str(), path == m_endpoint.data()))
				ConnectEndpoint(path);
			if (ImGui::BeginItemTooltip())
			{
				ImGui::TextWrapped("%s", path.c_str());
				ImGui::EndTooltip();
			}
		}
		if (!state.detail.empty())
			ImGui::TextWrapped("%s", state.detail.c_str());
		for (const auto& session : sessions)
		{
			const auto label = "Session " + std::to_string(session.id) + " / run " + std::to_string(session.generation);
			if (ImGui::Selectable(label.c_str(),
								  session.id == selected.id && session.generation == selected.generation))
				ChooseSession(session);
		}
		if (ImGui::CollapsingHeader("Advanced connection"))
		{
			ImGui::SetNextItemWidth(-1);
			ImGui::InputText("##Endpoint", m_endpoint.data(), m_endpoint.size());
			if (ImGui::Button("Connect to endpoint"))
				m_requests.Connect(m_endpoint.data());
			ImGui::TextWrapped("Queued: %zu; replies: %zu; exchange: %s", state.queued, state.ready,
							   state.inFlight ? "pending" : "idle");
		}
		if (!m_detail.empty())
		{
			ImGui::Separator();
			ImGui::TextWrapped("%s", m_detail.c_str());
			if (ImGui::SmallButton("Dismiss"))
				m_detail.clear();
		}
		ImGui::EndPopup();
	}

  private:
	using Work = PanelWork;
	void SelectionChanged()
	{
		StopPilot(true);
		m_devices.ClearHeldInputs();
		m_manualInput.SetTarget({});
		m_observation = 0;
		m_devices.ResetObservations();
		m_spatial.Reset();
		m_followOnResume = false;
		m_following = true;
		m_preview.Follow(true);
		m_stepRequested = false;
		m_stepped.reset();
		m_historySelection.reset();
		m_simulationState = {};
		m_simulationResult = {};
		m_manualInput.Refresh();
		m_workspace.SetScope(m_debugger.Scope());
		m_preview.SetScope(m_debugger.Scope());
	}
	void Reply(const PanelReply& response)
	{
		const auto& task = response.task;
		const auto& reply = response.response;
		const auto& message = reply.packet.message;
		const auto selected = m_debugger.Selected();
		if (task.session && (task.session != selected.id || task.generation != selected.generation))
		{
			if (task.kind == Work::Retain || task.kind == Work::Preview)
				m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId, U64(message, tag::ImageReaderId));
			if (task.kind == Work::Inspect)
				m_requests.Release(wire::OpCode::ReleaseInspection, tag::InspectionId, U64(message, tag::InspectionId));
			if (task.kind == Work::Acquire && U64(message, tag::Lease))
				m_requests.Send(wire::OpCode::Relinquish, Work::Release,
								{wire::Field::U64(tag::Session, task.session),
								 wire::Field::U64(tag::Generation, task.generation),
								 wire::Field::U64(tag::Lease, U64(message, tag::Lease))});
			return;
		}
		const auto error = U32(message, tag::Error, static_cast<std::uint32_t>(wire::ErrorCode::Internal));
		if (!reply.error.empty() || error)
		{
			const auto failure =
				!reply.error.empty() ? reply.error : Text(message, tag::Detail, "Runtime rejected the operation");
			if (!(task.kind == Work::Preview &&
				  (error == static_cast<std::uint32_t>(wire::ErrorCode::Busy) ||
				   error == static_cast<std::uint32_t>(wire::ErrorCode::UnsupportedCommand))))
				m_detail = failure;
			m_debugger.Failed(task, static_cast<wire::ErrorCode>(error), failure);
			m_workspace.SetScope(m_debugger.Scope(), false);
			m_preview.SetScope(m_debugger.Scope());
			if (task.kind == Work::Share)
				m_preview.ShareFailed(task.id);
			m_exports.Failed(task, failure);
			return;
		}
		const auto previous = m_debugger.Scope();
		if (m_debugger.Handle(task, message))
		{
			if (previous != m_debugger.Scope())
				SelectionChanged();
			for (const auto& effect : m_debugger.TakeCaptures())
			{
				if (effect.refresh && !m_requests.Has(Work::Capture, effect.capture))
				{
					auto fields = m_debugger.SessionFields();
					fields.push_back(wire::Field::U64(tag::CaptureId, effect.capture, true));
					m_requests.Send(wire::OpCode::Capture, Work::Capture, std::move(fields), effect.capture);
				}
				if (effect.released)
					m_workspace.MarkReleased(effect.capture);
				else if (m_workspace.Add(effect.capture, effect.frame))
				{
					// Pausing keeps the frame it stopped on.
					if (effect.paused && (U64(m_debugger.Status(), tag::Capabilities) &
										  static_cast<std::uint64_t>(wire::Capability::SnapshotPins)))
						m_debugger.Command(wire::CommandKind::PinSnapshot, effect.capture);
					// Following while paused: the newest stepped frame is the live edge.
					if (effect.frame && m_autoSelect && m_following)
					{
						Select(effect.capture);
						const auto scope = m_debugger.Scope();
						m_stepped = SteppedFrame{{{scope.session, scope.generation, effect.frame}, scope.epoch},
												 std::exchange(m_stepRequested, false)};
					}
				}
			}
			return;
		}
		if (m_exports.Handle(task, message))
			return;
		if (m_workspace.Handle(task, message))
		{
			if (task.kind == Work::Capture)
			{
				if (Boolean(message, tag::ImageAvailable))
				{
					auto fields = m_debugger.SessionFields();
					fields.push_back(wire::Field::U64(tag::CaptureId, task.id, true));
					m_requests.Send(wire::OpCode::RetainImage, Work::Retain, std::move(fields), task.id);
				}
				else if (m_workspace.SelectedId() == task.id)
					m_preview.ClearSelection();
			}
			return;
		}
		switch (task.kind)
		{
		case Work::FrameHistory:
			if (auto skipped = m_history.Accept(message); !skipped.empty() && m_historyNotice.empty())
				m_detail = m_historyNotice = "Frame history page skipped: " + skipped;
			break;
		case Work::Retain:
		case Work::Preview:
			m_preview.Retained(message, task.kind == Work::Preview);
			break;
		case Work::Share:
			m_preview.Shared(task.id, reply);
			break;
		default:
			break;
		}
	}
	void PollRangeExports()
	{
		if (m_rangeImages.empty()) return;
		const auto finish = [&](std::string result)
		{
			m_rangeImageResult = std::move(result);
			m_rangeImages.clear();
			m_preview.ProtectExports({});
		};
		if (m_rangeImageEpoch != m_requests.Epoch() || m_requests.Snapshot().state != RuntimeConnectionState::Connected)
		{
			finish("Snapshot export stopped: connection changed");
			return;
		}
		if (m_exports.Busy()) return;
		if (m_rangeImagePending)
		{
			if (!m_exports.Succeeded()) { finish(m_exports.Detail()); return; }
			++m_rangeImageIndex;
			m_rangeImagePending = false;
		}
		if (m_rangeImageIndex == m_rangeImages.size())
		{
			finish("Saved " + std::to_string(m_rangeImageIndex) + " snapshots");
			return;
		}
		const auto& image = m_rangeImages[m_rangeImageIndex];
		m_rangeImagePending = m_exports.Request(image.first, image.second);
		if (!m_rangeImagePending) finish("Snapshot export could not start");
	}
	void DrawRange(const RangeSel& range, ShellState& shell)
	{
		const auto& history = m_history.History();
		const auto first = history.Find(range.first, range.epoch), last = history.Find(range.last, range.epoch);
		ImGui::SeparatorText("Range");
		ImGui::Text("Run %llu / frames %llu – %llu", static_cast<unsigned long long>(range.first.run),
					static_cast<unsigned long long>(range.first.frame),
					static_cast<unsigned long long>(range.last.frame));
		if (!first || !last || *last < *first)
		{
			ImGui::TextDisabled("Range expired");
			return;
		}
		ImGui::Text("%.3f seconds / %zu retained frames", history.At(*last).seconds - history.At(*first).seconds,
					*last - *first + 1);
		ImGui::TextDisabled("Device activity: not recorded");
		ImGui::BeginDisabled();
		ImGui::Button("Save as take");
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Requires continuous input history (D8)");
		if (ImGui::Button("Zoom to range"))
		{
			shell.time.first = history.At(*first).seconds;
			shell.time.last = history.At(*last).seconds;
			shell.time.zoomToRange = true;
		}
		ImGui::SetNextItemWidth(-1);
		ImGui::InputText("##Range export file", m_rangePath.data(), m_rangePath.size());
		ImGui::BeginDisabled(m_rangeExport.valid() || !m_rangeImages.empty() || m_exports.Busy());
		if (ImGui::Button("Export frames"))
		{
			std::vector<HistoryFrame> frames;
			frames.reserve(*last - *first + 1);
			for (auto index = *first; index <= *last; ++index)
				frames.push_back(history.At(index));
			m_rangeResult.clear();
			m_rangeImageResult.clear();
			m_rangeImages.clear();
			const auto prefix = std::filesystem::path(m_rangePath.data()).replace_extension().string();
			std::vector<std::uint64_t> protectedImages;
			for (const auto& image : m_preview.Images())
				if (!image.live && image.ready && !image.released && image.reader && image.scope.epoch == range.epoch &&
					image.scope.epoch == m_requests.Epoch() && image.scope.session == range.first.session &&
					image.scope.generation == range.first.run && image.frame >= range.first.frame && image.frame <= range.last.frame)
				{
					protectedImages.push_back(image.id);
					m_rangeImages.push_back({image.reader, prefix + "-frame-" + std::to_string(image.frame) + "-image-" + std::to_string(image.id) + ".ppm"});
				}
			m_rangeImageEpoch = m_requests.Epoch();
			m_rangeImageIndex = 0;
			m_rangeImagePending = false;
			m_preview.ProtectExports(protectedImages);
			if (m_rangeImages.empty()) m_rangeImageResult = "No exportable snapshots in range";
			m_rangeExport = std::async(
				std::launch::async,
				[frames = std::move(frames), path = std::string(m_rangePath.data())]
				{
					std::ofstream output(path);
					output << "epoch,session,run,frame,run_seconds,display_interval_ms,snapshot,preview,dropped\n";
					for (const auto& frame : frames)
						output << frame.epoch << ',' << frame.identity.session << ',' << frame.identity.run << ','
							   << frame.identity.frame << ',' << frame.runSeconds << ',' << frame.frameMilliseconds
							   << ',' << bool(frame.flags & HistorySnapshot) << ','
							   << bool(frame.flags & HistoryPreview) << ',' << bool(frame.flags & HistoryDropped)
							   << '\n';
					output.close();
					return output ? "Saved frame metadata: " + path : "Could not save frame metadata: " + path;
				});
		}
		ImGui::EndDisabled();
		if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
			ImGui::SetTooltip("Export frame metadata as CSV and retained snapshots as PPM");
		if (m_rangeExport.valid() && m_rangeExport.wait_for(std::chrono::seconds(0)) == std::future_status::ready)
			m_rangeResult = m_rangeExport.get();
		if (!m_rangeResult.empty())
			ImGui::TextWrapped("%s", m_rangeResult.c_str());
		if (!m_rangeImageResult.empty()) ImGui::TextWrapped("%s", m_rangeImageResult.c_str());
	}
	void DrawFramePage(ShellState& shell)
	{
		// The page shows exactly the selected frame; an empty FrameSel means the time cursor.
		auto selection = std::get<FrameSel>(shell.selection);
		if (!selection.identity.frame)
			selection = {shell.time.cursor, shell.time.epoch};
		const auto& identity = selection.identity;
		const auto selectedCapture = m_workspace.Selected();
		const auto capture = selectedCapture && selectedCapture->scope.epoch == selection.epoch &&
									 selectedCapture->scope.session == identity.session &&
									 selectedCapture->scope.generation == identity.run &&
									 selectedCapture->frame == identity.frame
								 ? selectedCapture
								 : nullptr;
		const auto field = capture ? capture->document.Find(tag::Document) : nullptr;
		const auto bytes = field ? std::span<const std::byte>{field->value} : std::span<const std::byte>{};
		m_nodesDrawn = 0;
		m_documentEpoch = capture ? capture->scope.epoch : 0;
		ImGui::Text("Frame %llu", static_cast<unsigned long long>(identity.frame));
		// The header stays visible while the frame contents scroll.
		ImGui::BeginChild("Frame details", {0, 0});
		if (!bytes.empty())
		{
			auto rigSelection = shell.selection;
			m_spatial.DrawRig(rigSelection, {ImGui::GetContentRegionAvail().x / (ImGui::GetFontSize() / 16), 150});
		}
		ImGui::SeparatorText("Images");
		bool hasImage = false;
		for (const auto& image : m_preview.Images())
			if (image.scope.epoch == selection.epoch && image.scope.session == identity.session &&
				image.scope.generation == identity.run && image.frame == identity.frame &&
				image.ready && !image.released)
			{
				hasImage = true;
				ImGui::PushID(static_cast<int>(image.id));
				ImGui::Text("%s / %u x %u", image.live ? "Preview" : "Snapshot", image.description.width,
							image.description.height);
				m_preview.DrawImage(image.id, std::min(ImGui::GetContentRegionAvail().x, ImGui::GetFontSize() * 8), 0);
				if (ImGui::IsItemClicked())
				{
					m_preview.SelectImage(image.id);
					m_previewView = 0;
				}
				ImGui::PopID();
			}
		if (!hasImage)
			ImGui::TextDisabled("No image");
		if (capture && !capture->released && capture->scope == m_debugger.Scope())
		{
			const bool pinned = Boolean(capture->metadata, tag::Pinned);
			if (ImGui::SmallButton(pinned ? "Unpin snapshot" : "Pin snapshot"))
				m_debugger.Command(pinned ? wire::CommandKind::UnpinSnapshot : wire::CommandKind::PinSnapshot,
								   capture->id);
		}
		ImGui::SeparatorText("Input at this frame");
		if (!bytes.empty())
		{
			if (const auto tracking = ReadInspectionTracking(capture->document))
			{
				const auto pose = [](const char* name, const PoseSample& value)
				{
					const auto& p = value.pose.position;
					ImGui::Text("%s  %.2f, %.2f, %.2f m", name, p.x, p.y, p.z);
				};
				pose("Head", tracking->head);
				pose("Left", tracking->controllers[0].pose);
				pose("Right", tracking->controllers[1].pose);
			}
			const auto input = InspectionMember(bytes, "input");
			const auto controllers = InspectionElements(InspectionMember(input, "controllers"));
			for (std::size_t hand = 0; hand < std::min<std::size_t>(2, controllers.size()); ++hand)
			{
				const auto& controls = controllers[hand];
				ImGui::Text("%s  Trigger %.2f / Grip %.2f", hand ? "Right" : "Left",
							InspectionScalar(controls, "trigger").AsF32().value_or(0),
							InspectionScalar(controls, "grip").AsF32().value_or(0));
				ImGui::Text("A %s / B %s",
							InspectionScalar(controls, "primary").AsBool().value_or(false) ? "down" : "up",
							InspectionScalar(controls, "secondary").AsBool().value_or(false) ? "down" : "up");
			}
			if (ImGui::CollapsingHeader("All input values"))
				Document(input);
			const auto commands = InspectionMember(bytes, "applied_commands");
			wire::Message commandNode;
			commandNode.fields = wire::DecodeFields(commands).fields;
			const auto revisionId = U64(commandNode, 3);
			if (revisionId && m_documentEpoch == m_requests.Epoch())
			{
				if (const auto revision = m_workspace.Revision(revisionId))
				{
					const auto setup = ReadObservedSetup(*revision);
					const auto owner = [&](const char* label, ControlChannel channel)
					{
						const auto& source = setup.owners[static_cast<std::size_t>(channel)];
						const char* name = !source										 ? "Unowned"
										   : source->kind == CommandSourceKind::Manual	 ? "Manual"
										   : source->kind == CommandSourceKind::Rig		 ? "Script"
										   : source->kind == CommandSourceKind::Sequence ? "Sequence"
																						 : "MCP";
						ImGui::Text("%s owner: %s", label, name);
					};
					owner("Head", ControlChannel::Head);
					owner("Left", ControlChannel::LeftControls);
					owner("Right", ControlChannel::RightControls);
				}
				else
					m_workspace.RequestRevision(revisionId, m_documentEpoch);
			}
			if (ImGui::CollapsingHeader("Input owners and commands"))
				Document(commands);
		}
		else
			ImGui::TextDisabled(capture ? "Loading frame details" : "Input not retained");
		ImGui::SeparatorText("Actions");
		if (!bytes.empty())
		{
			const auto actions = InspectionMember(bytes, "actions");
			const auto entries = InspectionElements(InspectionMember(actions, "actions"));
			const auto dictionaryId = InspectionScalar(actions, "dictionary").AsU64().value_or(0);
			std::vector<std::vector<std::byte>> definitions;
			if (const auto dictionary = m_workspace.Revision(dictionaryId))
			{
				if (const auto document = dictionary->Find(tag::Document))
					definitions = InspectionElements(InspectionMember(document->value, "actions"));
			}
			else if (dictionaryId)
				m_workspace.RequestRevision(dictionaryId, m_documentEpoch);
			ImGui::Text("%zu actions", entries.size());
			for (std::size_t index = 0; index < std::min<std::size_t>(8, entries.size()); ++index)
			{
				const auto& action = entries[index];
				const auto id = InspectionScalar(action, "id").AsU64().value_or(0);
				std::string name = "Action " + std::to_string(id);
				for (const auto& definition : definitions)
					if (InspectionScalar(definition, "id").AsU64() == id)
					{
						const auto field = InspectionScalar(definition, "name");
						if (field.AsText())
							name = std::string(*field.AsText());
						break;
					}
				ImGui::PushID(static_cast<int>(index));
				if (ImGui::TreeNode(name.c_str()))
				{
					for (const auto& value : InspectionElements(InspectionMember(action, "values")))
					{
						ImGui::Text("%s / bool %s / %.3f",
									InspectionScalar(value, "active").AsBool().value_or(false) ? "Active" : "Inactive",
									InspectionScalar(value, "boolean").AsBool().value_or(false) ? "true" : "false",
									InspectionScalar(value, "scalar").AsF32().value_or(0));
						Document(value);
					}
					Document(InspectionMember(action, "bindings"));
					ImGui::TreePop();
				}
				ImGui::PopID();
			}
			if (ImGui::CollapsingHeader("All actions and bindings"))
				Document(actions);
		}
		else
			ImGui::TextDisabled("Actions not retained");
		if (ImGui::CollapsingHeader("Layers") && !bytes.empty())
			Document(InspectionMember(bytes, "layers"));
		if (ImGui::CollapsingHeader("Timing"))
		{
			if (const auto index = m_history.History().Find(identity, selection.epoch))
			{
				const auto& frame = m_history.History().At(*index);
				ImGui::Text("Run time %.3f s", frame.runSeconds);
				ImGui::Text("Display interval %.3f ms", frame.frameMilliseconds);
			}
			if (!bytes.empty())
			{
				Document(InspectionMember(bytes, "prediction"));
				Document(InspectionMember(bytes, "frame_trace"));
			}
		}
		if (ImGui::CollapsingHeader("Raw") && !bytes.empty())
		{
			if (ImGui::Button("Open in window"))
			{
				m_rawDocument.assign(bytes.begin(), bytes.end());
				m_rawEpoch = m_documentEpoch;
				m_rawOpen = true;
			}
			Document(bytes);
		}
		ImGui::EndChild();
	}
	void Document(std::span<const std::byte> bytes, unsigned depth = 0)
	{
		if (bytes.empty())
			return;
		if (depth > 20 || ++m_nodesDrawn > 2048)
		{
			ImGui::TextUnformatted("Expand fewer fields to inspect this document");
			return;
		}
		auto decoded = wire::DecodeFields(bytes);
		wire::Message node;
		node.fields = std::move(decoded.fields);
		if (decoded.error != wire::DecodeError::None)
		{
			ImGui::TextUnformatted("Invalid inspection node");
			return;
		}
		const auto name = Text(node, 1, "");
		const auto type = static_cast<wire::InspectionType>(U32(node, 2));
		const auto data = node.Find(3);
		if (!data)
			return;
		const auto label = name.empty() ? "state" : name.c_str();
		ImGui::PushID(static_cast<int>(m_nodesDrawn));
		if (type == wire::InspectionType::Object || type == wire::InspectionType::Array)
		{
			if (ImGui::TreeNode(label))
			{
				const auto entries = wire::DecodeFields(data->value);
				for (const auto& child : entries.fields)
					if (type == wire::InspectionType::Object)
						Document(child.value, depth + 1);
					else
						for (const auto& item : wire::DecodeFields(child.value).fields)
							Document(item.value, depth + 1);
				ImGui::TreePop();
			}
		}
		else if (type == wire::InspectionType::Revision)
		{
			const auto id = data->AsU64().value_or(0);
			ImGui::Text("%s: revision %llu", label, static_cast<unsigned long long>(id));
			ImGui::SameLine();
			ImGui::BeginDisabled(m_documentEpoch != m_requests.Epoch());
			if (ImGui::SmallButton("Inspect revision"))
				m_workspace.RequestRevision(id, m_documentEpoch);
			ImGui::EndDisabled();
			if (m_documentEpoch == m_requests.Epoch())
				if (const auto revision = m_workspace.Revision(id))
					if (auto document = revision->Find(tag::Document))
						Document(document->value, depth + 1);
		}
		else if (type == wire::InspectionType::Text)
			ImGui::TextWrapped("%s: %s", label, data->AsText() ? std::string(*data->AsText()).c_str() : "invalid text");
		else if (type == wire::InspectionType::Unsigned)
			ImGui::Text("%s: %llu", label, static_cast<unsigned long long>(data->AsU64().value_or(0)));
		else if (type == wire::InspectionType::Signed)
			ImGui::Text("%s: %lld", label, static_cast<long long>(data->AsI64().value_or(0)));
		else if (type == wire::InspectionType::Float)
			ImGui::Text("%s: %.7g", label, static_cast<double>(data->AsF32().value_or(0)));
		else if (type == wire::InspectionType::Boolean)
			ImGui::Text("%s: %s", label, data->AsBool().value_or(false) ? "true" : "false");
		else
			ImGui::Text("%s: unavailable", label);
		ImGui::PopID();
	}
	void StopPilot(bool discard)
	{
		m_pilot.Stop(m_window, m_manualInput, discard);
		m_devices.SetRigPiloting(false);
	}
	void SubmitEdit(const SimulationEdit& edit)
	{
		const auto selected = m_debugger.Selected();
		if (!selected.id || !selected.running)
		{
			m_detail = "Connect to a running session before editing simulated devices";
			return;
		}
		if (!m_manualInput.Submit(edit))
			m_detail = "Input rejected — release inputs";
	}

	void DeviceControls(Selection& selection)
	{
		const auto selected = m_debugger.Selected();
		const auto& status = m_debugger.Status();
		const bool supported = selected.id && (U64(status, tag::Capabilities) &
											   static_cast<std::uint64_t>(wire::Capability::SimulationCommands));
		m_nodesDrawn = 0;
		const auto phase = SimulationEditPhase(m_simulationResult);
		if (ImGui::SmallButton(phase.empty() ? "Input details" : ("Input: " + phase + "##Input details").c_str()))
			ImGui::OpenPopup("Input details");
		ImGui::SetNextWindowSizeConstraints({240, 0}, ImGui::GetMainViewport()->WorkSize);
		if (ImGui::BeginPopup("Input details"))
		{
			if (const auto value = m_simulationResult.Find(tag::Document))
			{
				ImGui::SeparatorText("Latest request");
				Document(value->value);
			}
			if (const auto value = m_simulationState.Find(tag::Document))
			{
				ImGui::SeparatorText("Applied state and input owners");
				Document(value->value);
			}
			ImGui::EndPopup();
		}
		m_spatial.DrawInspector(selection);
		m_devices.SetRigPiloting(m_pilot.Active() || m_spatial.Active());
		if (!m_preview.Following())
			return;
		if (auto edit = m_devices.Draw(supported, &selection))
		{
			StopPilot(true);
			SubmitEdit(*edit);
		}
		if (auto device = std::get_if<DeviceSel>(&selection); device && device->page == DevicePage::Trackers)
			device->slot = static_cast<unsigned>(m_devices.SelectedTracker());
	}

  public:
	void ConfigureSnapshots(std::uint32_t budgetMiB)
	{
		SnapshotBudget::Configure(budgetMiB);
		const auto& status = m_debugger.Status();
		SnapshotBudget::ObserveImageBytes(U64(status, tag::SnapshotImageBytes));
		if (!(U64(status, tag::Capabilities) & static_cast<std::uint64_t>(wire::Capability::SnapshotPins)) ||
			m_debugger.Pending())
			return;
		const auto scope = m_debugger.Scope();
		if (scope == m_budgetScope && m_budgetMiB == budgetMiB)
			return;
		if (U32(status, tag::SnapshotBudgetMiB) != budgetMiB)
			m_debugger.Command(wire::CommandKind::ConfigureSnapshots, budgetMiB);
		m_budgetScope = scope;
		m_budgetMiB = budgetMiB;
	}

	void SetPreviewExpanded(bool expanded)
	{
		m_previewExpanded = expanded;
	}

  private:
	PanelScope m_budgetScope;
	std::uint32_t m_budgetMiB = 0;
	bool m_previewExpanded = false;
	bool m_panning = false;
	FrameHistoryFeed m_history;
	std::vector<HistoryFrame> m_coverage;
	std::optional<HistoryFrame> m_historySelection;
	std::optional<SteppedFrame> m_stepped;
	std::string m_historyNotice;
	bool m_following = true, m_stepRequested = false;
	Clock::time_point m_nextHistory{};
	RuntimePanelRequests m_requests;
	DebuggerPanelController m_debugger;
	InspectionWorkspace m_workspace;
	PreviewImages m_preview;
	ImageExportTask m_exports;
	std::array<char, 256> m_endpoint{};
	std::vector<std::string> m_endpoints;
	Window* m_window = nullptr;
	std::uint64_t m_observation = 0, m_appliedPilotUpdates = 0;
	wire::Message m_simulationState, m_simulationResult;
	ManualInputController m_manualInput;
	RecordingPanel m_recording;
	bool m_replayActive = false;
	struct RecordingReplay
	{
		automation::SavedRecording saved;
		PanelScope scope;
	};
	std::optional<RecordingReplay> m_recordingReplay;
	PlayerRig m_playerRig;
	PilotController m_pilot{m_playerRig};
	DeviceControlPanel m_devices{m_playerRig};
	SpatialEditor m_spatial{m_playerRig, m_manualInput};
	Clock::time_point m_nextPreview{}, m_nextCapture{};
	std::string m_detail;
	std::array<char, 512> m_exportFile{};
	std::array<char, 512> m_rangePath{"frames.csv"};
	std::future<std::string> m_rangeExport;
	std::string m_rangeResult, m_rangeImageResult;
	std::vector<std::pair<std::uint64_t, std::string>> m_rangeImages;
	std::uint64_t m_rangeImageEpoch = 0;
	std::size_t m_rangeImageIndex = 0;
	bool m_rangeImagePending = false;
	bool m_rawOpen = false;
	std::uint64_t m_rawEpoch = 0;
	std::vector<std::byte> m_rawDocument;
	bool m_autoSelect = true, m_focusPreview = true, m_fitPreview = true;
	bool m_followOnResume = false;
	std::uint64_t m_resumeAfter = 0;
	float m_zoom = 1;
	ImVec2 m_previousImageSize{};
	int m_previewView = 0;
	int m_rawLayer = 0, m_rawView = 0;
	std::uint32_t m_nodesDrawn = 0;
	std::uint64_t m_documentEpoch = 0;
};
RuntimePanels::RuntimePanels(UiRenderer& renderer, std::string endpoint, Window* window)
	: m_impl(std::make_unique<Impl>(renderer, endpoint, window))
{
}
RuntimePanels::~RuntimePanels() = default;
void RuntimePanels::FocusLost()
{
	m_impl->FocusLost();
}
void RuntimePanels::ConfigureSnapshots(std::uint32_t budgetMiB)
{
	m_impl->ConfigureSnapshots(budgetMiB);
}
void RuntimePanels::ConfigurePilot(const PilotBindings& keys, const GamepadBindings& gamepad)
{
	m_impl->ConfigurePilot(keys, gamepad);
}
void RuntimePanels::Poll()
{
	m_impl->Poll();
}
void RuntimePanels::Draw(ShellState& shell)
{
	m_impl->Draw(shell);
}
void RuntimePanels::DrawConnection(bool* open)
{
	m_impl->DrawConnection(open);
}
std::vector<VkSemaphore> RuntimePanels::ReadyWaits() const
{
	return m_impl->ReadyWaits();
}
void RuntimePanels::Submitted(std::uint64_t serial)
{
	m_impl->Submitted(serial);
}
RuntimePanelSnapshot RuntimePanels::Snapshot() const
{
	return m_impl->Snapshot();
}
void RuntimePanels::Command(control::CommandKind kind, std::uint64_t capture, bool raw, std::uint32_t layer,
							std::uint32_t view)
{
	m_impl->Command(kind, capture, raw, layer, view);
}
void RuntimePanels::Pause()
{
	m_impl->Pause();
}
void RuntimePanels::Step()
{
	m_impl->Step();
}
std::optional<SteppedFrame> RuntimePanels::TakeSteppedFrame()
{
	return m_impl->TakeSteppedFrame();
}
void RuntimePanels::TogglePin(std::size_t index)
{
	m_impl->TogglePin(index);
}
void RuntimePanels::MarkHistoryFrame(std::size_t index, bool enabled)
{
	m_impl->MarkHistoryFrame(index, enabled);
}
void RuntimePanels::SelectHistoryFrame(std::size_t index)
{
	m_impl->SelectHistoryFrame(index);
}
const HistoryStore& RuntimePanels::History() const
{
	return m_impl->History();
}
std::vector<RuntimeFrame> RuntimePanels::TimelineFrames() const
{
	return m_impl->TimelineFrames();
}
void RuntimePanels::SelectFrame(std::uint64_t local)
{
	m_impl->SelectFrame(local);
}
void RuntimePanels::SelectCapture(std::uint64_t capture)
{
	m_impl->SetLive(false);
	m_impl->Select(capture);
}
void RuntimePanels::ReleaseSelected()
{
	m_impl->ReleaseSelected();
}
void RuntimePanels::CancelPending()
{
	m_impl->CancelPending();
}
void RuntimePanels::ReleaseInputs()
{
	m_impl->ReleaseInputs();
}
std::string RuntimePanels::Endpoint() const
{
	return m_impl->Endpoint();
}
void RuntimePanels::ConnectEndpoint(std::string endpoint)
{
	m_impl->ConnectEndpoint(std::move(endpoint));
}
void RuntimePanels::StartRecording()
{
	m_impl->StartRecording();
}
void RuntimePanels::StopRecording()
{
	m_impl->StopRecording();
}
void RuntimePanels::SetReplayActive(bool active)
{
	m_impl->SetReplayActive(active);
}
void RuntimePanels::ResumeLive()
{
	m_impl->ResumeLive();
}
void RuntimePanels::SetLive(bool enabled)
{
	m_impl->SetLive(enabled);
}
std::vector<std::uint64_t> RuntimePanels::CaptureIds() const
{
	return m_impl->CaptureIds();
}
void RuntimePanels::InspectSelected()
{
	m_impl->InspectSelected();
}

void RuntimePanels::RequestExport(std::string path)
{
	m_impl->RequestExport(std::move(path));
}
std::optional<automation::SavedRecording> RuntimePanels::TakeRecordingReplay()
{
	return m_impl->TakeRecordingReplay();
}
} // namespace simxr::app

void simxr::app::RuntimePanels::SetPreviewExpanded(bool expanded)
{
	m_impl->SetPreviewExpanded(expanded);
}
