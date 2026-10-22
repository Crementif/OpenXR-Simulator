#include "app/RuntimeSession.h"
#include "app/RuntimeEndpoints.h"
#include "control/SimulationControl.h"
#include "graphics/SnapshotBudget.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace simxr::app
{
namespace
{
namespace wire = control;
namespace tag = wire::tag;
using panel::Boolean;
using panel::Nested;
using panel::Text;
using panel::U32;
using panel::U64;
using Work = PanelWork;
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
bool SameFrame(const PanelScope& scope, std::uint64_t frame, const HistoryFrame& history)
{
	return scope.epoch == history.epoch && scope.session == history.identity.session &&
		   scope.generation == history.identity.run && frame == history.identity.frame;
}
} // namespace
RuntimeSession::RuntimeSession(UiRenderer& renderer, const std::string& endpoint)
	: m_debugger(m_requests), m_workspace(m_requests), m_preview(renderer, m_requests), m_exports(m_requests),
	  m_endpoints(DiscoverRuntimeEndpoints()), m_endpoint(endpoint)
{
	if (!endpoint.empty())
		m_requests.Connect(endpoint);
}
RuntimeSession::~RuntimeSession()
{
	m_requests.Close();
}
bool RuntimeSession::Connected() const
{
	return m_requests.Snapshot().state == RuntimeConnectionState::Connected;
}
bool RuntimeSession::Supports(wire::Capability capability) const
{
	return U64(m_debugger.Status(), tag::Capabilities) & static_cast<std::uint64_t>(capability);
}
bool RuntimeSession::SupportsEdits() const
{
	return m_debugger.Selected().id && Supports(wire::Capability::SimulationCommands);
}
void RuntimeSession::Notify(std::string title, std::string detail)
{
	m_notices.push_back({std::move(title), std::move(detail)});
}
void RuntimeSession::Notify(Notice notice)
{
	m_notices.push_back(std::move(notice));
}
std::vector<Notice> RuntimeSession::TakeNotices()
{
	return std::exchange(m_notices, {});
}
bool RuntimeSession::TakeScopeChanged()
{
	return std::exchange(m_scopeChanged, false);
}
bool RuntimeSession::TakeObservation()
{
	return std::exchange(m_observationChanged, false);
}
void RuntimeSession::ScopeChanged()
{
	// §11.3: a new session or run goes live and keeps nothing of the old input state.
	m_scopeChanged = true;
	m_manualInput.SetTarget({});
	m_observation = 0;
	m_setup.reset();
	m_input = {};
	m_followOnResume = m_stepRequested = false;
	m_stepped.reset();
	m_time = {};
	m_manualInput.Refresh();
	m_workspace.SetScope(m_debugger.Scope());
	m_preview.SetScope(m_debugger.Scope());
	m_preview.ShowLive(true);
}
void RuntimeSession::ChooseSession(PanelSession session)
{
	m_debugger.ChooseSession(session);
	ScopeChanged();
}
void RuntimeSession::Poll(bool piloting)
{
	if (m_requests.RefreshEpoch())
	{
		m_debugger.ResetEpoch();
		ScopeChanged();
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
			Notify("Runtime reply not understood", error.what());
		}
	if (Connected())
	{
		m_requests.FlushReleases();
		m_debugger.Poll();
		const auto selected = m_debugger.Selected();
		const auto now = Clock::now();
		m_history.SetScope(m_debugger.Scope());
		if (selected.id && now >= m_nextHistory && !m_requests.Has(Work::FrameHistory) &&
			Supports(wire::Capability::FrameHistory))
		{
			auto fields = m_debugger.SessionFields();
			fields.push_back(wire::Field::U64(tag::Offset, m_history.After()));
			m_requests.Send(wire::OpCode::ReadFrameHistory, Work::FrameHistory, std::move(fields));
			m_nextHistory = now + std::chrono::milliseconds(100);
		}
		m_manualInput.SetTarget(SupportsEdits()
									? ManualInputTarget{m_requests.Snapshot().path, selected.id, selected.generation}
									: ManualInputTarget{});
		const auto previousError = m_input.error;
		m_input = m_manualInput.Snapshot();
		if (m_observation != m_input.observation)
		{
			m_observation = m_input.observation;
			m_observationChanged = true;
			m_setup.reset();
			if (m_input.state.Find(tag::Document))
				try
				{
					m_setup = ReadObservedSetup(m_input.state);
				}
				catch (const std::exception&)
				{
					// The runtime has not published its device configuration yet.
				}
		}
		if (!m_input.error.empty() && m_input.error != previousError)
			Notify("Input rejected", m_input.error);
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
		if (m_preview.ShowingLive() && selected.id && now >= m_nextPreview && !m_requests.Has(Work::Preview) &&
			!m_preview.HasPendingLive() && Supports(wire::Capability::LivePreview))
		{
			m_requests.Send(wire::OpCode::RetainPreview, Work::Preview, m_debugger.SessionFields());
			m_nextPreview = now + std::chrono::milliseconds(piloting ? 16 : 33);
		}
	}
	else
		m_manualInput.SetTarget({});
	if (m_followOnResume)
		for (const auto& operation : m_debugger.Operations())
			if (operation.local > m_resumeAfter && operation.kind == wire::CommandKind::Resume &&
				panel::Terminal(operation.phase))
			{
				if (operation.phase != "complete")
					m_followOnResume = false;
				else if (!m_debugger.Manual() && Connected())
				{
					m_preview.ShowLive(m_time.following);
					m_followOnResume = false;
				}
			}
	m_exports.Poll();
	m_preview.Poll();
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
			if (const auto index =
					history.Find({image.scope.session, image.scope.generation, image.frame}, image.scope.epoch))
			{
				const auto frame = history.At(*index);
				history.SetCoverage(*index, image.capture ? image.capture : frame.capture,
									frame.flags | (image.live ? HistoryPreview : HistorySnapshot));
				m_coverage.push_back(frame);
			}
	for (const auto& shot : m_preview.History().Shots())
		if (shot.frame.frame)
			if (const auto index = history.Find(shot.frame, shot.epoch))
			{
				m_coverage.push_back(history.At(*index));
				history.SetFlags(*index, HistoryPreview, true);
			}
	for (const auto& capture : m_workspace.Captures())
		if (!capture.released && Boolean(capture.metadata, tag::Pinned))
			if (const auto index = history.Find({capture.scope.session, capture.scope.generation, capture.frame},
												capture.scope.epoch))
			{
				m_coverage.push_back(history.At(*index));
				history.SetFlags(*index, HistoryPinned, true);
			}
	if (auto error = m_requests.TakeError(); !error.empty())
		m_failure = std::move(error);
	for (auto error : {m_debugger.TakeError(), m_workspace.TakeError(), m_preview.TakeError()})
		if (!error.empty())
			Notify("Runtime request failed", std::move(error));
}
void RuntimeSession::Reply(const PanelReply& response)
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
		if (!(task.kind == Work::Preview && (error == static_cast<std::uint32_t>(wire::ErrorCode::Busy) ||
											 error == static_cast<std::uint32_t>(wire::ErrorCode::UnsupportedCommand))))
			Notify(task.kind == Work::Preview ? "Preview image unavailable" : "Runtime refused a request", failure);
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
			ScopeChanged();
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
				if (effect.paused && Supports(wire::Capability::SnapshotPins))
					m_debugger.Command(wire::CommandKind::PinSnapshot, effect.capture);
				// Following while paused: the newest stepped frame is the live edge.
				if (effect.frame && m_autoSelect && m_time.following)
				{
					ShowCapture(effect.capture);
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
		if (auto skipped = m_history.Accept(message); !skipped.empty() && m_historyNotice != skipped)
		{
			m_historyNotice = skipped;
			Notify("Frame history page skipped", std::move(skipped));
		}
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
void RuntimeSession::ShowCapture(std::uint64_t capture)
{
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
void RuntimeSession::SelectCapture(std::uint64_t capture)
{
	m_time.following = false;
	for (const auto& record : m_workspace.Captures())
		if (record.id == capture && record.scope == m_debugger.Scope())
		{
			m_time.cursor = {record.scope.session, record.scope.generation, record.frame};
			m_time.epoch = record.scope.epoch;
		}
	ShowCapture(capture);
}
void RuntimeSession::SelectFrame(std::uint64_t local)
{
	for (const auto& capture : m_workspace.Captures())
		if (capture.local == local)
		{
			m_time = {false, {capture.scope.session, capture.scope.generation, capture.frame}, capture.scope.epoch};
			if (capture.scope == m_debugger.Scope())
				ShowCapture(capture.id);
			else
			{
				m_workspace.SelectLocal(local);
				if (!m_preview.SelectCapture(capture.scope, capture.id))
					m_preview.ClearSelection();
			}
			return;
		}
}
void RuntimeSession::SelectImage(std::uint64_t id)
{
	for (const auto& image : m_preview.Images())
		if (image.id == id)
		{
			m_time = {false, {image.scope.session, image.scope.generation, image.frame}, image.scope.epoch};
			m_preview.SelectImage(id);
			m_workspace.SelectImage(image.scope, image.capture);
		}
}
void RuntimeSession::LookAt(std::size_t index)
{
	const auto frame = m_history.History().At(index);
	m_time = {false, frame.identity, frame.epoch};
	for (const auto& capture : m_workspace.Captures())
		if (SameFrame(capture.scope, capture.frame, frame))
		{
			SelectFrame(capture.local);
			return;
		}
	m_preview.ClearSelection();
	m_preview.ShowLive(false);
	m_workspace.SelectImage({}, 0);
	for (const auto& image : m_preview.Images())
		if (image.ready && !image.released && SameFrame(image.scope, image.frame, frame))
		{
			m_preview.SelectImage(image.id);
			break;
		}
}
void RuntimeSession::MarkHistoryFrame(std::size_t index, bool enabled)
{
	m_history.History().SetFlags(index, HistoryMarker, enabled);
}
void RuntimeSession::TogglePin(std::size_t index)
{
	const auto& frame = m_history.History().At(index);
	for (const auto& capture : m_workspace.Captures())
		if (!capture.released && capture.scope == m_debugger.Scope() && SameFrame(capture.scope, capture.frame, frame))
		{
			const bool pinned = Boolean(capture.metadata, tag::Pinned);
			m_debugger.Command(pinned ? wire::CommandKind::UnpinSnapshot : wire::CommandKind::PinSnapshot,
							   capture.id);
			return;
		}
}
void RuntimeSession::Command(wire::CommandKind kind, std::uint64_t capture, bool raw, std::uint32_t layer,
							 std::uint32_t view)
{
	if (kind == wire::CommandKind::Pause || kind == wire::CommandKind::Step)
		m_followOnResume = false;
	m_debugger.Command(kind, capture, raw, layer, view);
}
void RuntimeSession::Pause()
{
	Command(wire::CommandKind::Pause);
}
void RuntimeSession::Step()
{
	m_time.following = true;
	m_stepRequested = true;
	Command(wire::CommandKind::Step);
}
std::optional<SteppedFrame> RuntimeSession::TakeSteppedFrame()
{
	return std::exchange(m_stepped, {});
}
void RuntimeSession::ResumeLive()
{
	m_time.following = true;
	m_resumeAfter = m_debugger.Operations().empty() ? 0 : m_debugger.Operations().back().local;
	m_followOnResume = true;
	m_debugger.Command(wire::CommandKind::Resume);
}
void RuntimeSession::SetLive(bool enabled)
{
	if (!enabled && m_time.following)
	{
		// Looking back starts at the frame on screen.
		const auto view = View();
		m_time.cursor = view.selectedIdentity.frame ? view.selectedIdentity : view.time.cursor;
		m_time.epoch = view.selectedIdentity.frame ? view.selectedEpoch : view.time.epoch;
	}
	m_time.following = enabled;
	m_preview.ShowLive(enabled);
}
void RuntimeSession::CancelPending()
{
	m_debugger.CancelPending();
}
void RuntimeSession::ReleaseSelected()
{
	if (const auto capture = m_workspace.Selected();
		capture && !capture->released && capture->scope == m_debugger.Scope())
		m_debugger.Command(wire::CommandKind::Release, capture->id);
	m_workspace.ReleaseSelected();
	m_preview.ReleaseSelected();
}
void RuntimeSession::RequestExport(std::string path)
{
	const auto image = m_preview.Selected();
	if (!image || !image->reader || image->scope.epoch != m_requests.Epoch() ||
		!m_exports.Request(image->reader, std::move(path)))
		Notify("Image not exportable", "Retain an exact source image before requesting export");
}
void RuntimeSession::ConnectEndpoint(std::string endpoint)
{
	m_endpoint = endpoint;
	m_failure.clear();
	m_requests.Connect(std::move(endpoint));
}
void RuntimeSession::Disconnect()
{
	m_requests.Disconnect();
}
void RuntimeSession::RefreshEndpoints()
{
	m_endpoints = DiscoverRuntimeEndpoints();
}
std::string RuntimeSession::Endpoint() const
{
	return m_endpoint;
}
bool RuntimeSession::SubmitEdit(const SimulationEdit& edit)
{
	const auto selected = m_debugger.Selected();
	if (!selected.id || !selected.running)
	{
		Notify("No running session", "Connect to a running session before editing simulated devices");
		return false;
	}
	if (!m_manualInput.Submit(edit))
	{
		Notify("Input rejected", "Release inputs and try again");
		return false;
	}
	return true;
}
void RuntimeSession::ConfigureSnapshots(std::uint32_t budgetMiB)
{
	SnapshotBudget::Configure(budgetMiB);
	const auto& status = m_debugger.Status();
	SnapshotBudget::ObserveImageBytes(U64(status, tag::SnapshotImageBytes));
	if (!Supports(wire::Capability::SnapshotPins) || m_debugger.Pending())
		return;
	const auto scope = m_debugger.Scope();
	if (scope == m_budgetScope && m_budgetMiB == budgetMiB)
		return;
	if (U32(status, tag::SnapshotBudgetMiB) != budgetMiB)
		m_debugger.Command(wire::CommandKind::ConfigureSnapshots, budgetMiB);
	m_budgetScope = scope;
	m_budgetMiB = budgetMiB;
}
std::vector<RuntimeFrame> RuntimeSession::TimelineFrames() const
{
	std::vector<RuntimeFrame> frames;
	const auto images = m_preview.Images();
	const auto selected = m_workspace.Selected();
	for (const auto& capture : m_workspace.Captures())
	{
		if (!capture.frame)
			continue;
		const bool imageReady = std::any_of(images.begin(), images.end(),
											[&](const auto& image)
											{
												return image.scope == capture.scope && image.capture == capture.id &&
													   image.frame == capture.frame && image.ready && !image.released;
											});
		frames.push_back({{capture.scope.epoch, capture.scope.session, capture.scope.generation},
						  capture.local,
						  capture.id,
						  capture.frame,
						  !m_time.following && selected && selected->local == capture.local,
						  imageReady,
						  capture.document.Find(tag::Document) != nullptr,
						  !capture.released && capture.scope == m_debugger.Scope() && Connected()});
	}
	return frames;
}
RuntimeView RuntimeSession::View() const
{
	RuntimeView view;
	const auto& history = m_history.History();
	if (history.Size())
		view.historySeconds = history.At(history.Size() - 1).seconds;
	const auto connection = m_requests.Snapshot();
	const auto selected = m_debugger.Selected();
	view.connected = connection.state == RuntimeConnectionState::Connected;
	view.connectionState = ConnectionName(connection.state);
	view.endpoint = connection.path;
	view.sessionAvailable = selected.id != 0;
	view.sessionRunning = selected.running;
	view.epoch = m_requests.Epoch();
	view.latestFrame = U64(Nested(m_debugger.Status().Find(tag::Presentation)), tag::LastSubmitted);
	view.session = selected.id;
	view.generation = selected.generation;
	view.manual = m_debugger.Manual();
	const auto retention = Nested(m_debugger.Status().Find(tag::Retention));
	const auto capacity = U64(retention, tag::Capacity);
	view.historyFull = capacity && U64(retention, tag::Pinned) >= capacity;
	view.time = m_time;
	if (m_time.following)
	{
		view.time.cursor = {selected.id, selected.generation, view.latestFrame};
		view.time.epoch = view.epoch;
	}
	view.selectedCapture = m_workspace.SelectedId();
	if (const auto capture = m_workspace.Selected())
		view.selectedPinned = Boolean(capture->metadata, tag::Pinned);
	view.captures = m_workspace.Captures().size();
	view.images = m_preview.ReadyCount();
	view.appliedPilotUpdates = m_input.appliedPoses;
	view.setup = m_setup;
	view.pending = m_requests.Pending() || m_exports.Busy();
	view.error = !m_failure.empty()								 ? m_failure
				 : connection.state == RuntimeConnectionState::Failed ? connection.detail
																	   : std::string{};
	view.operationPending = m_debugger.Pending();
	if (!m_debugger.Operations().empty())
	{
		const auto& operation = m_debugger.Operations().back();
		view.lastOperation = operation.id;
		view.phase = operation.phase;
		view.progress = operation.progress;
	}
	if (!m_workspace.Captures().empty())
		view.lastCapture = m_workspace.Captures().back().id;
	if (const auto image = m_preview.Selected())
	{
		view.selectedEpoch = image->scope.epoch;
		view.selectedIdentity = {image->scope.session, image->scope.generation, image->frame};
		view.selectedFrame = image->frame;
		view.selectedImageCapture = image->capture;
		view.selectedReader = image->reader;
		view.selectedReady = image->ready;
		view.selectedLayers = image->description.arrayLayers;
		view.selectedViews = image->description.views;
	}
	if (const auto capture = m_workspace.Selected(); capture && capture->document.Find(tag::Document))
	{
		view.inspectedFrame = panel::Frame(capture->document);
		if (!view.selectedFrame && !m_time.following)
		{
			view.selectedEpoch = capture->scope.epoch;
			view.selectedFrame = capture->frame;
			view.selectedIdentity = {capture->scope.session, capture->scope.generation, capture->frame};
		}
	}
	if (!view.selectedFrame && !m_time.following)
	{
		view.selectedEpoch = m_time.epoch;
		view.selectedIdentity = m_time.cursor;
		view.selectedFrame = m_time.cursor.frame;
	}
	// Frames per second over the last half second of this run's display intervals.
	if (history.Size() && history.At(history.Size() - 1).identity.run == selected.generation)
	{
		double milliseconds = 0;
		std::size_t frames = 0;
		for (auto index = history.Size(); index-- > 0 && frames < 45;)
		{
			const auto& frame = history.At(index);
			if (frame.identity.run != selected.generation || frame.flags & HistoryRunStart)
				break;
			milliseconds += frame.frameMilliseconds;
			++frames;
		}
		if (frames && milliseconds > 0 && !view.manual)
			view.fps = float(1000.0 * double(frames) / milliseconds);
	}
	return view;
}
} // namespace simxr::app
