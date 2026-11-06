#include "app/services/RuntimeSession.h"
#include "app/UiPch.h"
#include "app/services/RuntimeEndpoints.h"
#include "graphics/SnapshotBudget.h"

namespace simxr::app
{
namespace wire = control;

namespace tag = wire::tag;

using panel::Boolean;

using panel::U32;

using panel::U64;

using Work = PanelWork;

RuntimeSession::RuntimeSession(UiRenderer& renderer, const std::string& endpoint)
	: m_debugger(m_requests), m_workspace(m_requests), m_preview(renderer, m_requests), m_exports(m_requests),
	  m_endpoint(endpoint)
{
	RefreshEndpoints();
	if (!endpoint.empty())
		m_requests.Connect(endpoint);
}

RuntimeSession::~RuntimeSession()
{
	m_requests.Close();
}

void RuntimeSession::Poll(bool piloting, bool foreground)
{
	const auto connection = m_requests.Snapshot();
	if (connection.state == RuntimeConnectionState::Failed)
	{
		// An app that ends its connection leaves the session disconnected, not failed.
		if (!connection.peerClosed)
			m_failure = connection.detail.empty() ? "Runtime connection failed" : connection.detail;
		// Retire the failed epoch, including replies queued before the transport failed.
		m_requests.Disconnect();
	}
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
			if (!foreground && (reply.task.kind == Work::Preview || reply.task.kind == Work::Thumbnail))
			{
				m_requests.Release(wire::OpCode::ReleaseImage, tag::ImageReaderId,
								   U64(reply.response.packet.message, tag::ImageReaderId));
				continue;
			}
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
		if (foreground && !m_time.following && m_time.epoch == m_debugger.Scope().epoch &&
			m_time.cursor.session == selected.id && m_time.cursor.run == selected.generation && m_time.cursor.frame &&
			m_unavailablePreview != m_time.cursor.frame && !m_workspace.SelectedId() &&
			!m_requests.Has(Work::PastPreview) && Supports(wire::Capability::ContinuousPreviews))
		{
			const auto image = m_preview.Selected();
			if ((!image || !image->reader || !PastState()) &&
				(!m_preview.SelectFrame(m_debugger.Scope(), m_time.cursor.frame) || !PastState()))
			{
				auto fields = m_debugger.SessionFields();
				fields.push_back(wire::Field::U64(tag::FrameId, m_time.cursor.frame, true));
				m_requests.Send(wire::OpCode::RetainPreview, Work::PastPreview, std::move(fields), m_time.cursor.frame);
			}
		}
		m_history.SetScope(m_debugger.Scope());
		if (foreground && selected.id && selected.running && Supports(wire::Capability::LiveHaptics) &&
			now >= m_nextHaptics && !m_requests.Has(Work::Haptics))
		{
			m_requests.Send(wire::OpCode::ReadHaptics, Work::Haptics, m_debugger.SessionFields());
			m_hapticsRequested = now;
			m_nextHaptics = now + std::chrono::milliseconds(33);
		}
		if (selected.id && m_history.CanRead() && now >= m_nextHistory && !m_requests.Has(Work::FrameHistory) &&
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
		if (foreground && selected.id && Supports(wire::Capability::ContinuousPreviews) &&
			!m_requests.Has(Work::PastPreview) && !m_requests.Has(Work::Thumbnail) && !m_preview.HasPendingThumbnail())
		{
			const auto wanted = m_preview.History().MissingThumbnail();
			const auto scope = m_debugger.Scope();
			if (wanted && wanted->epoch == scope.epoch && wanted->frame.session == scope.session &&
				wanted->frame.run == scope.generation)
			{
				auto fields = m_debugger.SessionFields();
				fields.push_back(wire::Field::U64(tag::FrameId, wanted->frame.frame, true));
				m_requests.Send(wire::OpCode::RetainPreview, Work::Thumbnail, std::move(fields), wanted->frame.frame);
			}
		}
		if (foreground && m_preview.ShowingLive() && selected.id && now >= m_nextPreview &&
			!m_requests.Has(Work::Preview) && !m_preview.HasPendingLive() && Supports(wire::Capability::LivePreview))
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
	m_coverage.Begin();
	if (Supports(wire::Capability::ContinuousPreviews))
	{
		const auto scope = m_debugger.Scope();
		// Retained previews can predate this companion's metadata feed. They remain
		// available in the runtime, but have no local timeline row to annotate yet.
		if (const auto bounds = history.FrameBounds({scope.session, scope.generation, 0}, scope.epoch))
		{
			const auto add = [&](std::uint64_t frame) {
				if (frame >= bounds->first && frame <= bounds->second)
					m_coverage.Add({scope.session, scope.generation, frame}, scope.epoch, 0, HistoryPreview);
			};
			if (const auto packed = m_debugger.Status().Find(tag::PreviewFrameIds))
			{
				for (std::size_t offset = 0; offset + 8 <= packed->value.size(); offset += 8)
				{
					std::uint64_t frame = 0;
					for (unsigned byte = 0; byte < 8; ++byte)
						frame |= std::uint64_t{std::to_integer<unsigned char>(packed->value[offset + byte])}
								 << (byte * 8);
					add(frame);
				}
			}
			else
				for (const auto& field : panel::Nested(m_debugger.Status().Find(tag::PreviewFrames)).fields)
					if (const auto frame = field.AsU64())
						add(*frame);
		}
	}
	for (const auto& image : m_preview.Images())
		if (image.ready && !image.released)
			m_coverage.Add({image.scope.session, image.scope.generation, image.frame}, image.scope.epoch, image.capture,
						   image.live ? HistoryPreview : HistorySnapshot);
	for (const auto& shot : m_preview.History().Shots())
		if (shot.frame.frame)
			m_coverage.Add(shot.frame, shot.epoch, 0, HistoryPreview);
	for (const auto& capture : m_workspace.Captures())
		if (!capture.released && Boolean(capture.metadata, tag::Pinned))
			m_coverage.Add({capture.scope.session, capture.scope.generation, capture.frame}, capture.scope.epoch, 0,
						   HistoryPinned);
	m_coverage.Apply(history);
	if (auto error = m_requests.TakeError(); !error.empty())
		m_failure = std::move(error);
	for (auto error : {m_debugger.TakeError(), m_workspace.TakeError(), m_preview.TakeError()})
		if (!error.empty())
			Notify("Runtime request failed", std::move(error));
}

bool RuntimeSession::TakeScopeChanged()
{
	return std::exchange(m_scopeChanged, false);
}

std::vector<Notice> RuntimeSession::TakeNotices()
{
	return std::exchange(m_notices, {});
}

void RuntimeSession::Notify(std::string title, std::string detail)
{
	m_notices.push_back({std::move(title), std::move(detail)});
}

void RuntimeSession::Notify(Notice notice)
{
	m_notices.push_back(std::move(notice));
}

void RuntimeSession::ConnectEndpoint(std::string endpoint, std::optional<std::uint64_t> expectedProcess)
{
	m_endpoint = endpoint;
	m_failure.clear();
	m_requests.Connect(std::move(endpoint), expectedProcess);
}

void RuntimeSession::Disconnect()
{
	m_requests.Disconnect();
}

void RuntimeSession::RefreshEndpoints()
{
	try
	{
		m_endpoints = DiscoverRuntimeEndpoints();
	}
	catch (const std::exception& error)
	{
		Notify("Application discovery failed", error.what());
	}
}

std::string RuntimeSession::Endpoint() const
{
	return m_endpoint;
}

void RuntimeSession::ChooseSession(PanelSession session)
{
	m_debugger.ChooseSession(session);
	ScopeChanged();
}

void RuntimeSession::ConfigureSnapshots(std::uint32_t budgetMiB)
{
	SnapshotBudget::Configure(budgetMiB);
	const auto& status = m_debugger.Status();
	const auto capacity = SnapshotBudget::CapacityFor(budgetMiB, U64(status, tag::SnapshotImageBytes));
	m_preview.SetCapacity(capacity);
	m_workspace.SetCapacity(capacity);
	if (!Connected() || !Supports(wire::Capability::SnapshotPins) || m_debugger.Pending() ||
		U32(status, tag::SnapshotBudgetMiB) == budgetMiB)
		return;
	const auto scope = m_debugger.Scope();
	const auto now = Clock::now();
	if (scope == m_budgetScope && m_budgetMiB == budgetMiB && now < m_nextBudget)
		return;
	m_debugger.Command(wire::CommandKind::ConfigureSnapshots, budgetMiB);
	m_budgetScope = scope;
	m_budgetMiB = budgetMiB;
	m_nextBudget = now + std::chrono::seconds(1);
}

void RuntimeSession::ConfigurePreviews(std::uint32_t budgetMiB, std::uint32_t ramMiB)
{
	if (!Connected() || !Supports(wire::Capability::ConfigurablePreviews) ||
		(U64(m_debugger.Status(), tag::PreviewBudget) == (std::uint64_t{budgetMiB} << 20) &&
		 (!m_debugger.Status().Find(tag::PreviewRamBudget) ||
		  U64(m_debugger.Status(), tag::PreviewRamBudget) == (std::uint64_t{ramMiB} << 20))) ||
		m_requests.Has(Work::ConfigurePreviews) || Clock::now() < m_nextPreviewBudget)
		return;
	auto fields = m_debugger.SessionFields();
	fields.push_back(wire::Field::U64(tag::PreviewBudget, std::uint64_t{budgetMiB} << 20, true));
	if (m_debugger.Status().Find(tag::PreviewRamBudget))
		fields.push_back(wire::Field::U64(tag::PreviewRamBudget, std::uint64_t{ramMiB} << 20, true));
	m_requests.Send(wire::OpCode::ConfigurePreviews, Work::ConfigurePreviews, std::move(fields));
	m_nextPreviewBudget = Clock::now() + std::chrono::seconds(1);
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

bool RuntimeSession::Connected() const
{
	return m_requests.Snapshot().state == RuntimeConnectionState::Connected;
}

bool RuntimeSession::SupportsEdits() const
{
	return m_debugger.Selected().id && Supports(wire::Capability::SimulationCommands);
}

bool RuntimeSession::Supports(wire::Capability capability) const
{
	return U64(m_debugger.Status(), tag::Capabilities) & static_cast<std::uint64_t>(capability);
}

bool RuntimeSession::TakeObservation()
{
	return std::exchange(m_observationChanged, false);
}

void RuntimeSession::ScopeChanged()
{
	// A new session or run goes live and keeps nothing of the old input state.
	m_scopeChanged = true;
	m_manualInput.SetTarget({});
	m_observation = 0;
	m_haptics.Reset();
	m_hapticFailure.clear();
	m_historyNotice.clear();
	m_nextHaptics = {};
	m_setup.reset();
	m_input = {};
	m_followOnResume = m_stepRequested = false;
	m_stepped.reset();
	m_time = {};
	m_unavailablePreview = 0;
	m_pastState = {};
	m_manualInput.Refresh();
	m_workspace.SetScope(m_debugger.Scope());
	m_preview.SetScope(m_debugger.Scope());
	m_preview.ClearSelection();
	m_preview.ShowLive(true);
}
} // namespace simxr::app
