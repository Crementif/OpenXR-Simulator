#include "Pch.h"
#include "app/services/RuntimeSession.h"

namespace simxr::app
{
namespace wire = control;
namespace tag = wire::tag;
using panel::Boolean;
using panel::Nested;
using panel::U64;
using Work = PanelWork;
static const char* ConnectionName(RuntimeConnectionState state)
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
static bool SameFrame(const PanelScope& scope, std::uint64_t frame, const HistoryFrame& history)
{
	return scope.epoch == history.epoch && scope.session == history.identity.session &&
		   scope.generation == history.identity.run && frame == history.identity.frame;
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
	m_unavailablePreview = 0;
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
bool RuntimeSession::LoadingPastImage() const
{
	const auto image = m_preview.Selected();
	return m_requests.Has(Work::PastPreview) || (image && !image->ready && !image->released);
}
std::uint64_t RuntimeSession::PreviewBytes() const
{
	return Supports(wire::Capability::ContinuousPreviews) ? U64(m_debugger.Status(), tag::PreviewBytes)
														  : m_preview.History().Bytes();
}
std::uint64_t RuntimeSession::PreviewBudget() const
{
	return Supports(wire::Capability::ContinuousPreviews) ? U64(m_debugger.Status(), tag::PreviewBudget)
														  : m_preview.History().Budget();
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
			m_debugger.Command(pinned ? wire::CommandKind::UnpinSnapshot : wire::CommandKind::PinSnapshot, capture.id);
			return;
		}
}
void RuntimeSession::Command(wire::CommandKind kind, std::uint64_t capture, bool raw, std::uint32_t layer,
							 std::uint32_t view)
{
	if (kind == wire::CommandKind::Pause || kind == wire::CommandKind::Step)
		m_followOnResume = false;
	if (kind == wire::CommandKind::Pause || kind == wire::CommandKind::Step || kind == wire::CommandKind::Resume ||
		kind == wire::CommandKind::Cancel)
		m_stepRequested = false;
	m_debugger.Command(kind, capture, raw, layer, view);
}
void RuntimeSession::Pause()
{
	Command(wire::CommandKind::Pause);
}
void RuntimeSession::Step()
{
	m_time.following = true;
	Command(wire::CommandKind::Step);
	m_stepRequested = true;
}
std::optional<SteppedFrame> RuntimeSession::TakeSteppedFrame()
{
	return std::exchange(m_stepped, {});
}
void RuntimeSession::ResumeLive()
{
	m_time.following = true;
	m_stepRequested = false;
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
	m_stepRequested = false;
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
bool RuntimeSession::RequestExport(std::string path)
{
	const auto image = m_preview.Selected();
	if (!image || !image->reader || image->scope.epoch != m_requests.Epoch() ||
		!m_exports.Request(image->reader, std::move(path)))
	{
		Notify("Image not exportable", "Retain an exact source image before requesting export");
		return false;
	}
	return true;
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
SessionView RuntimeSession::View() const
{
	SessionView view;
	const auto& history = m_history.History();
	if (history.Size())
		view.historySeconds = history.At(history.Size() - 1).seconds;
	const auto connection = m_requests.Snapshot();
	const auto selected = m_debugger.Selected();
	view.connected = connection.state == RuntimeConnectionState::Connected;
	view.connectionState = ConnectionName(connection.state);
	view.endpoint = connection.path;
	view.appName = connection.appName;
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
	view.error = !m_failure.empty()									  ? m_failure
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
	// Frames per second over the latest application intervals in this run.
	if (view.connected && selected.running && history.Size())
	{
		double milliseconds = 0;
		std::size_t frames = 0;
		for (auto index = history.Size(); index-- > 0 && frames < 45;)
		{
			const auto& frame = history.At(index);
			if (frame.epoch != view.epoch || frame.identity.session != selected.id ||
				frame.identity.run != selected.generation || frame.flags & HistoryRunStart)
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
