#include "app/RuntimePanels.h"
#include "app/DebuggerPanelController.h"
#include "app/DeviceControlPanel.h"
#include "app/ImageExportTask.h"
#include "app/InspectionWorkspace.h"
#include "app/PreviewImages.h"
#include "app/RecordingPanel.h"
#include "app/RuntimeConnection.h"
#include "app/RuntimeEndpoints.h"
#include "app/UiRenderer.h"
#include "control/ImageInspection.h"
#include "control/InspectionDocument.h"
#include "control/SimulationControl.h"

#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <filesystem>
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
	Impl(UiRenderer& renderer, const std::string& path)
		: m_debugger(m_requests), m_workspace(m_requests), m_preview(renderer, m_requests), m_exports(m_requests),
		  m_endpoints(DiscoverRuntimeEndpoints())
	{
		std::snprintf(m_endpoint.data(), m_endpoint.size(), "%s", path.c_str());
		std::snprintf(m_exportFile.data(), m_exportFile.size(), "%s", "snapshot.ppm");
		if (!path.empty())
			m_requests.Connect(path);
	}
	~Impl()
	{
		m_requests.Close();
	}
	void ChooseSession(PanelSession session)
	{
		m_debugger.ChooseSession(session);
		SelectionChanged();
	}
	void Select(std::uint64_t capture)
	{
		m_focusPreview = true;
		const bool canFetch = m_workspace.Select(capture);
		// Releasing the remote capture does not invalidate an already-owned GPU
		// copy. Select that private image before considering another source read.
		if (m_preview.SelectCapture(m_debugger.Scope(), capture) || !canFetch)
			return;
		auto fields = m_debugger.SessionFields();
		fields.push_back(wire::Field::U64(tag::CaptureId, capture, true));
		m_requests.Send(wire::OpCode::Capture, Work::Capture, std::move(fields), capture);
	}
	void Command(wire::CommandKind kind, std::uint64_t capture = 0, bool raw = false, std::uint32_t layer = 0,
				 std::uint32_t view = 0)
	{
		m_debugger.Command(kind, capture, raw, layer, view);
	}
	void CancelPending()
	{
		m_debugger.CancelPending();
	}
	void ReleaseInputs()
	{
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
		if (!image || !image->reader || image->scope.epoch != m_requests.Epoch() ||
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
		const auto connection = m_requests.Snapshot();
		const auto selected = m_debugger.Selected();
		snapshot.connected = connection.state == RuntimeConnectionState::Connected;
		snapshot.connectionState = ConnectionName(connection.state);
		snapshot.endpoint = connection.path;
		snapshot.sessionAvailable = selected.id != 0;
		snapshot.session = selected.id;
		snapshot.generation = selected.generation;
		snapshot.manual = m_debugger.Manual();
		snapshot.selectedCapture = m_workspace.SelectedId();
		snapshot.captures = m_workspace.Captures().size();
		snapshot.images = m_preview.ReadyCount();
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
			snapshot.selectedFrame = image->frame;
			snapshot.selectedImageCapture = image->capture;
			snapshot.selectedReader = image->reader;
			snapshot.selectedReady = image->ready;
			snapshot.selectedLayers = image->description.arrayLayers;
		}
		if (const auto capture = m_workspace.Selected(); capture && capture->document.Find(tag::Document))
			snapshot.inspectedFrame = panel::Frame(capture->document);
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
			m_transaction = 1;
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
			if (selected.id && now >= m_nextSimulation && !m_requests.Has(Work::SimulationStatus) &&
				(U64(m_debugger.Status(), tag::Capabilities) &
				 static_cast<std::uint64_t>(wire::Capability::SimulationCommands)))
			{
				m_requests.Send(wire::OpCode::SimulationStatus, Work::SimulationStatus, SimulationFields());
				m_nextSimulation = now + std::chrono::milliseconds(500);
			}
			if (const auto capture = m_workspace.Selected(); capture && !capture->released &&
				capture->scope == m_debugger.Scope() && now >= m_nextCapture &&
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
				m_nextPreview = now + std::chrono::milliseconds(100);
			}
		}
		m_exports.Poll();
		m_preview.Poll();
		for (auto error :
			 {m_requests.TakeError(), m_debugger.TakeError(), m_workspace.TakeError(), m_preview.TakeError()})
			if (!error.empty())
				m_detail = std::move(error);
	}
	void Draw(WorkspaceMode mode)
	{
		if (mode == WorkspaceMode::Simulator || mode == WorkspaceMode::Custom)
			DeviceControls();
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
#ifdef SIMXR_COMPANION_AUTOMATION
		constexpr bool replayAvailable = true;
#else
		constexpr bool replayAvailable = false;
#endif
		if (mode == WorkspaceMode::Automation || mode == WorkspaceMode::Custom)
			if (auto replay = m_recording.Draw(canRecord, replayAvailable))
				m_recordingReplay = RecordingReplay{std::move(*replay), m_debugger.Scope()};
		m_preview.BeginFrame();
		const auto state = m_requests.Snapshot();
		if (mode == WorkspaceMode::Debug || mode == WorkspaceMode::Custom)
		{
			ImGui::Begin("Frame controls");
			ImGui::BeginDisabled(!selected.id || state.state != RuntimeConnectionState::Connected);
			if (ImGui::Button("Pause"))
				Command(wire::CommandKind::Pause);
			ImGui::SameLine();
			if (ImGui::Button("Step"))
				Command(wire::CommandKind::Step);
			ImGui::SameLine();
			if (ImGui::Button("Resume"))
				Command(wire::CommandKind::Resume);
			if (ImGui::Button("Cancel pending operation"))
				CancelPending();
			if (ImGui::Button("Display step"))
				Command(wire::CommandKind::DisplayStep);
			ImGui::SameLine();
			if (ImGui::Button("Strict admission"))
				Command(wire::CommandKind::StrictAdmission);
			ImGui::SetNextItemWidth(140);
			ImGui::InputInt("Layer", &m_rawLayer);
			ImGui::SetNextItemWidth(140);
			ImGui::InputInt("View", &m_rawView);
			if (ImGui::Button("Step source view"))
				Command(wire::CommandKind::Step, 0, true, static_cast<std::uint32_t>(std::max(0, m_rawLayer)),
						static_cast<std::uint32_t>(std::max(0, m_rawView)));
			ImGui::EndDisabled();
			if (!selected.id)
				ImGui::TextWrapped("Session unavailable; retained GPU snapshots remain usable.");
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
			ImGui::End();
			ImGui::Begin("Captures");
			ImGui::Checkbox("Select completed steps", &m_autoSelect);
			for (const auto& capture : captures)
			{
				const auto label = "Capture " + std::to_string(capture.id) + " / frame " +
								   std::to_string(capture.frame) + " / " + capture.phase +
								   (capture.released ? " (released)" : "");
				ImGui::PushID(static_cast<int>(capture.local));
				if (ImGui::Selectable(label.c_str(), capture.local == selectedRecord))
				{
					if (capture.scope.epoch == epoch && capture.scope.session == selected.id &&
						capture.scope.generation == selected.generation)
						Select(capture.id);
					else
					{
						m_workspace.SelectLocal(capture.local);
						m_preview.ClearSelection();
						m_preview.Follow(false);
					}
				}
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
			if (ImGui::Button("Release selected snapshot"))
				ReleaseSelected();
			const auto capture = m_workspace.Selected();
			ImGui::BeginDisabled(!capture || capture->released || capture->scope.epoch != epoch ||
								 capture->scope.session != selected.id ||
								 capture->scope.generation != selected.generation);
			if (ImGui::Button("Inspect frame state"))
				m_workspace.InspectSelected();
			ImGui::EndDisabled();
			ImGui::End();
		}
		if (m_focusPreview)
		{
			ImGui::SetNextWindowFocus();
			m_focusPreview = false;
		}
		if (ImGui::Begin("GPU preview"))
		{
			const auto image = m_preview.Selected();
			const bool live = m_preview.Following();
			ImGui::TextColored(!connected ? ImVec4{0.65f, 0.68f, 0.72f, 1}
										  : (live ? ImVec4{0.35f, 0.85f, 0.65f, 1} : ImVec4{1, 0.75f, 0.35f, 1}),
							   "%s",
							   live ? (!connected ? (image ? "LAST LIVE FRAME / DISCONNECTED" : "NOT CONNECTED")
									   : manual	  ? "LIVE VIEW / PAUSED"
												  : "LIVE VIEW")
									: "RETAINED FRAME");
			if (!live)
			{
				ImGui::SameLine();
				if (ImGui::Button("Return to live"))
					m_preview.Follow(true);
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
				ImGui::SameLine();
				ImGui::TextDisabled("Frame %llu", static_cast<unsigned long long>(image->frame));
				ImGui::Checkbox("Fit", &m_fitPreview);
				ImGui::SameLine();
				ImGui::SetNextItemWidth(120);
				if (ImGui::SliderFloat("Zoom", &m_zoom, 0.25f, 16.0f, "%.2fx", ImGuiSliderFlags_Logarithmic))
					m_fitPreview = false;
				if (mode == WorkspaceMode::Debug || mode == WorkspaceMode::Custom)
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
						ImGui::TextWrapped("PPM stacks views vertically and includes frame identity; alpha is omitted. "
							"Use RGBA to preserve all four channels.");
					}
				}
				const auto available = ImGui::GetContentRegionAvail();
				const auto& description = image->description;
				float width = description.width * m_zoom;
				if (m_fitPreview)
				{
					const float slices = static_cast<float>(image->slices);
					width = std::max(16.0f,
									 std::min((available.x - 12 * (slices - 1)) / slices,
											  std::max(16.0f, available.y) * description.width / description.height));
				}
				ImGui::BeginChild("Image area", {0, 0}, 0, ImGuiWindowFlags_HorizontalScrollbar);
				m_preview.DrawSelected(width);
				ImGui::EndChild();
			}
			else
			{
				ImGui::Spacing();
				ImGui::SeparatorText(connected ? "Waiting for an image" : "Your application appears here");
				ImGui::TextWrapped(
					"%s",
					!connected	   ? "Choose an application above to launch it, or use Connect to select an already "
									 "running OpenXR application."
					: !selected.id ? "The runtime is connected. Start an XR session in the application to see its view."
					: m_preview.SelectedId()
						? "Receiving the selected frame..."
						: "The application has not supplied a visible frame. If it is paused, use Step or Resume.");
			}
		}
		ImGui::End();
		if (mode != WorkspaceMode::Debug && mode != WorkspaceMode::Custom)
			return;
		ImGui::Begin("Historical state");
		m_nodesDrawn = 0;
		if (auto capture = m_workspace.Selected(); capture && capture->document.Find(tag::Document))
		{
			m_documentEpoch = capture->scope.epoch;
			ImGui::Text("Capture %llu / frame %llu", static_cast<unsigned long long>(capture->id),
						static_cast<unsigned long long>(capture->frame));
			Document(capture->document.Find(tag::Document)->value);
		}
		else
			ImGui::TextWrapped("Select an exact capture and request its submitted CPU state. Actions, tracking and "
							   "layers reflect that historical frame.");
		ImGui::End();
		ImGui::Begin("Action and binding inspector");
		m_nodesDrawn = 0;
		ImGui::TextWrapped("Synchronized values show what the application observed at xrSyncActions. Raw input is a "
						   "separate snapshot. Inspecting never synchronizes or changes actions.");
		if (auto capture = m_workspace.Selected(); capture && capture->document.Find(tag::Document))
		{
			m_documentEpoch = capture->scope.epoch;
			ImGui::Text("Frame %llu / capture %llu", static_cast<unsigned long long>(capture->frame),
						static_cast<unsigned long long>(capture->id));
			const auto bytes = capture->document.Find(tag::Document)->value;
			const auto actions = InspectionMember(bytes, "actions"), input = InspectionMember(bytes, "input");
			ImGui::SeparatorText("Action sync, profiles, bindings and inactivity reasons");
			if (!actions.empty())
				Document(actions);
			ImGui::TextWrapped("Expand the dictionary revision for action-set priorities, named actions, subaction "
							   "paths and suggested bindings. Inactive reasons distinguish inactive sets, missing "
							   "bindings, unavailable sources and focus.");
			ImGui::SeparatorText("Raw simulated controls at submission");
			if (!input.empty())
				Document(input);
		}
		else
		{
			ImGui::TextWrapped("Select a retained frame and use Inspect submitted CPU state in Captures. This "
							   "inspector preserves the selected historical action-sync serial.");
		}
		ImGui::End();
	}

	void DrawConnection()
	{
		const auto selected = m_debugger.Selected();
		const auto sessions = m_debugger.Sessions();
		ImGui::SetNextWindowSize({600, 400}, ImGuiCond_FirstUseEver);
		if (!ImGui::Begin("Runtime connection"))
		{
			ImGui::End();
			return;
		}
		const auto state = m_requests.Snapshot();
		ImGui::TextUnformatted(ConnectionName(state.state));
		ImGui::TextWrapped("Select an existing runtime, or launch an application from the toolbar.");
		if (ImGui::Button("Refresh applications"))
			m_endpoints = DiscoverRuntimeEndpoints();
		if (state.state == RuntimeConnectionState::Connected)
		{
			ImGui::SameLine();
			if (ImGui::Button("Disconnect"))
				m_requests.Disconnect();
		}
		if (m_endpoints.empty())
			ImGui::TextWrapped("No runtimes found. Launch an application with the simulator runtime, then refresh.");
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
		ImGui::End();
	}

  private:
	using Work = PanelWork;
	void SelectionChanged()
	{
		m_simulationState = {};
		m_simulationResult = {};
		m_nextSimulation = {};
		m_workspace.SetScope(m_debugger.Scope());
		m_preview.SetScope(m_debugger.Scope());
	}
	Fields SimulationFields() const
	{
		auto fields = m_debugger.SessionFields();
		fields.push_back(wire::Field::U32(tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Manual)));
		return fields;
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
				if (effect.released)
					m_workspace.MarkReleased(effect.capture);
				else if (m_workspace.Add(effect.capture, effect.frame) && m_autoSelect)
					Select(effect.capture);
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
		case Work::Retain:
		case Work::Preview:
			m_preview.Retained(message, task.kind == Work::Preview);
			break;
		case Work::Share:
			m_preview.Shared(task.id, reply);
			break;
		case Work::Simulate:
			m_simulationResult = message;
			m_nextSimulation = {};
			break;
		case Work::SimulationStatus:
			m_simulationState = message;
			break;
		default:
			break;
		}
	}
	void Document(std::span<const std::byte> bytes, unsigned depth = 0)
	{
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
	void SubmitEdit(const SimulationEdit& edit)
	{
		const auto selected = m_debugger.Selected();
		if (!selected.id || !selected.running)
		{
			m_detail = "Connect to a running session before editing simulated devices";
			return;
		}
		try
		{
			auto packet =
				EncodeSimulationEdit(edit, selected.id, selected.generation, m_transaction, m_simulationState);
			const auto kind = std::holds_alternative<ManualTransaction>(edit) ? Work::Simulate : Work::SimulationStatus;
			if (m_requests.Send(static_cast<wire::OpCode>(packet.message.opcode), kind,
								std::move(packet.message.fields)) &&
				kind == Work::Simulate)
				++m_transaction;
		}
		catch (const std::exception& error)
		{
			m_detail = error.what();
		}
	}
	void DeviceControls()
	{
		const auto selected = m_debugger.Selected();
		const auto& status = m_debugger.Status();
		if (!ImGui::Begin("Device controls"))
		{
			ImGui::End();
			return;
		}
		const bool supported = selected.id && (U64(status, tag::Capabilities) &
											   static_cast<std::uint64_t>(wire::Capability::SimulationCommands));
		if (auto edit = m_devices.Draw(supported))
			SubmitEdit(*edit);
		m_nodesDrawn = 0;
		if (const auto value = m_simulationResult.Find(tag::Document))
		{
			const auto phaseBytes = InspectionMember(value->value, "phase");
			wire::Message phase;
			phase.fields = wire::DecodeFields(phaseBytes).fields;
			ImGui::SeparatorText("Input request");
			ImGui::TextWrapped("%s", Text(phase, 3, "Waiting for acknowledgement").c_str());
			if (ImGui::TreeNode("Request details"))
			{
				Document(value->value);
				ImGui::TreePop();
			}
		}
		if (const auto value = m_simulationState.Find(tag::Document);
			value && ImGui::CollapsingHeader("Applied state and input owners"))
		{
			ImGui::TextWrapped(
				"Owner source kinds: 0 Manual, 1 MCP, 2 Rig. Owner array follows the channel order shown above.");
			ImGui::SeparatorText("Observed owners and applied state");
			Document(value->value);
		}
		ImGui::End();
	}
	RuntimePanelRequests m_requests;
	DebuggerPanelController m_debugger;
	InspectionWorkspace m_workspace;
	PreviewImages m_preview;
	ImageExportTask m_exports;
	std::array<char, 256> m_endpoint{};
	std::vector<std::string> m_endpoints;
	wire::Message m_simulationState, m_simulationResult;
	std::uint64_t m_transaction = 1;
	RecordingPanel m_recording;
	struct RecordingReplay
	{
		automation::SavedRecording saved;
		PanelScope scope;
	};
	std::optional<RecordingReplay> m_recordingReplay;
	DeviceControlPanel m_devices;
	Clock::time_point m_nextSimulation{}, m_nextPreview{}, m_nextCapture{};
	std::string m_detail;
	std::array<char, 512> m_exportFile{};
	bool m_autoSelect = true, m_focusPreview = true, m_fitPreview = true;
	float m_zoom = 4;
	int m_rawLayer = 0, m_rawView = 0;
	std::uint32_t m_nodesDrawn = 0;
	std::uint64_t m_documentEpoch = 0;
};
RuntimePanels::RuntimePanels(UiRenderer& renderer, std::string endpoint)
	: m_impl(std::make_unique<Impl>(renderer, endpoint))
{
}
RuntimePanels::~RuntimePanels() = default;
void RuntimePanels::Poll()
{
	m_impl->Poll();
}
void RuntimePanels::Draw(WorkspaceMode mode)
{
	m_impl->Draw(mode);
}
void RuntimePanels::DrawConnection()
{
	m_impl->DrawConnection();
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
void RuntimePanels::SelectCapture(std::uint64_t capture)
{
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
