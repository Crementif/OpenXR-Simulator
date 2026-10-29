#include "control/ControlService.h"
#include "Pch.h"
#include "control/ConnectionImageWindows.h"
#include "control/ConnectionImages.h"
#include "control/ConnectionLeases.h"
#include "control/ConnectionRequests.h"
#include "control/ControlRequestError.h"
#include "control/ControlSchema.h"
#include "control/ControlSessions.h"
#include "control/DebugProtocol.h"
#include "control/FrameInspection.h"
#include "control/ImageInspection.h"
#include "control/SimulationConnection.h"
#include "runtime/Runtime.h"
#include "runtime/Session.h"
#include "simulation/GameBridge.h"

#include <new>

namespace simxr::control
{
static constexpr std::uint64_t s_commonCapabilities =
	static_cast<std::uint64_t>(Capability::DebugCommands) | static_cast<std::uint64_t>(Capability::SnapshotMetadata) |
	static_cast<std::uint64_t>(Capability::SnapshotPins) | static_cast<std::uint64_t>(Capability::PipelineInspection) |
	static_cast<std::uint64_t>(Capability::HistoricalCpuState) |
	static_cast<std::uint64_t>(Capability::ImmutableRevisions);

static ImageHistoryView HistoryView(const Message& request, const Session& session)
{
	const auto target = request.Find(tag::Target) ? DecodeCaptureTarget(request) : CaptureTarget{};
	if (target == CaptureTarget{})
		return ImageHistoryView::Composed;
	if (target == CaptureTarget{CaptureKind::QuadLayers} && session.SupportsCapture(target))
		return ImageHistoryView::Quads;
	throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported image-history target"};
}
static CaptureTarget HistoryTarget(ImageHistoryView view)
{
	return {view == ImageHistoryView::Quads ? CaptureKind::QuadLayers : CaptureKind::ComposedEyes};
}
static Message Response(const Message& request, ErrorCode error = ErrorCode::Ok, const char* detail = nullptr)
{
	Message response;
	response.opcode = request.opcode;
	response.flags = 1;
	response.minor = std::min(request.minor, ProtocolMinor);
	response.requestId = request.requestId;
	response.fields.push_back(Field::U32(tag::Error, static_cast<std::uint32_t>(error), true));
	if (detail)
		response.fields.push_back(Field::Text(tag::Detail, detail));
	return response;
}
static Field Nested(std::uint16_t name, std::vector<Field> fields)
{
	return Field::Bytes(name, EncodeFields(fields));
}
static std::uint64_t ImageCapabilitiesMask(ImageCapabilities capabilities, bool handles)
{
	return (capabilities.imageExport ? static_cast<std::uint64_t>(Capability::ImageExport) : 0) |
		   (capabilities.gpuPreview && handles ? static_cast<std::uint64_t>(Capability::GpuPreview) : 0);
}
struct ControlService::Client
{
	ConnectionId identity;
	std::unique_ptr<SimulationConnection> simulation;
	const bool nativeHandles;
	ConnectionLeases leases;
	InspectionStore inspections;
	ConnectionImages images;
	ConnectionImageWindows windows;
	// Destroy admission first so its close observer can still retire the
	// connection's leases and simulation source using live member owners.
	ConnectionRequests requests;
	Client(Runtime& runtime, bool nativeHandles)
		: nativeHandles(nativeHandles), requests(
											[this, &runtime]
											{
												Retire(runtime);
											})
	{
	}
	void Retire(Runtime& runtime) noexcept
	{
		auto retiredSimulation = std::move(simulation);
		leases.Close();
		if (identity.value)
			runtime.Game().Disconnect(identity.value, true);
	}
};

struct ControlService::Publication
{
	InspectionStore& store;
	ConnectionImageWindows& windows;
	ImageWindowReader& retiredWindow;
	std::optional<std::uint64_t> window;
	ConnectionImages::Publication images;
	std::optional<std::uint64_t> inspection;
	bool committed = false;
	Publication(Client& client, ConnectionImages::RetiredResource& retired, ImageWindowReader& retiredWindow)
		: store(client.inspections), windows(client.windows), retiredWindow(retiredWindow),
		  images(client.images, retired)
	{
	}
	~Publication()
	{
		if (window && !committed)
			retiredWindow = windows.Release(*window);
		if (inspection && !committed)
			store.Release(*inspection);
	}
	void Commit() noexcept
	{
		committed = true;
		images.Commit();
	}
};

struct ControlService::HeldReaders
{
	ControlSessions::Snapshot sessions;
	ImageHistorySnapshot history;
	ImageWindowReader window;
	ImageWindowReader retiredWindow;
	FrameImageHistory::StartedWindow startedWindow;
	HistoricalImage historicalImage;
	std::shared_ptr<DebugController> controller;
	std::optional<SnapshotRecord> capture;
	std::vector<SnapshotRecord> sequenceCaptures;
	std::optional<SnapshotRecord> selectedCapture;
	std::optional<DebugOperation> operation;
	std::optional<PresentationSnapshot> display;
	ConnectionImages::RetiredResource images;
	ConnectionLeases::Retired leases;
	static_assert(std::is_nothrow_move_assignable_v<decltype(capture)>);
	static_assert(std::is_nothrow_move_assignable_v<decltype(operation)>);
	static_assert(std::is_nothrow_move_assignable_v<decltype(display)>);
};

std::uint64_t ControlService::ServiceCapabilities(std::uint16_t minor) const
{
	auto capabilities = s_commonCapabilities;
	if (minor >= 4)
	{
		capabilities |= static_cast<std::uint64_t>(Capability::SimulationCommands) |
						static_cast<std::uint64_t>(Capability::CommandRecording) |
						static_cast<std::uint64_t>(Capability::FrameDiagnostics) |
						static_cast<std::uint64_t>(Capability::ProjectionHistory) |
						static_cast<std::uint64_t>(Capability::FrameHistory);
		if (m_runtime.Game().Snapshot().capabilities)
			capabilities |= static_cast<std::uint64_t>(Capability::GameBridge);
	}
	if (minor >= 5)
		capabilities |= static_cast<std::uint64_t>(Capability::PoseSequences);
	if (minor >= 7 && m_images)
		capabilities |= static_cast<std::uint64_t>(Capability::VideoExport);
	if (minor >= 15)
		capabilities |= static_cast<std::uint64_t>(Capability::GameObservations);
	if (minor >= 16 && m_images)
		capabilities |= static_cast<std::uint64_t>(Capability::ImageRegionExport);
	if (minor >= 14)
		capabilities |= static_cast<std::uint64_t>(Capability::RecordingDrain);
	if (minor >= 13)
		capabilities |= static_cast<std::uint64_t>(Capability::LiveBodyInput);
	if (minor >= 12)
		capabilities |= static_cast<std::uint64_t>(Capability::BodyReplayTiming);
	if (minor >= 11)
		capabilities |= static_cast<std::uint64_t>(Capability::BodyObservations);
	if (minor >= 10)
		capabilities |= static_cast<std::uint64_t>(Capability::TrackerCollections);
	if (minor >= 9)
		capabilities |= static_cast<std::uint64_t>(Capability::ExternalDisplayTiming);
	if (minor >= 8)
		capabilities |= static_cast<std::uint64_t>(Capability::LiveInput);
	if (minor >= 6)
		capabilities |= static_cast<std::uint64_t>(Capability::LiveHaptics);
	return capabilities;
}
std::uint64_t ControlService::AdvertisedCapabilities(const Client& client, std::uint16_t minor) const
{
	auto capabilities = ServiceCapabilities(minor);
	if (m_images && minor >= 2)
		capabilities |= ImageCapabilitiesMask({true, true}, client.nativeHandles);
	if (m_images && minor >= 3)
		capabilities |= static_cast<std::uint64_t>(Capability::LivePreview) |
						static_cast<std::uint64_t>(Capability::ContinuousPreviews) |
						static_cast<std::uint64_t>(Capability::ConfigurablePreviews);
	return capabilities;
}

std::shared_ptr<ControlService::Client> ControlService::Find(ConnectionId id)
{
	std::lock_guard lock(m_mutex);
	const auto entry = std::find_if(m_clients.begin(), m_clients.end(),
									[&](const auto& value)
									{
										return value.first == id;
									});
	return entry == m_clients.end() ? nullptr : entry->second;
}
const ControlSession& ControlService::Resolve(Client& client, const Message& request, HeldReaders& held)
{
	m_sessions.Discover(held.sessions);
	client.leases.Prune(held.sessions.Ids(), held.leases);
	return held.sessions.Resolve({RequireRequestU64(request, tag::Session)},
								 {RequireRequestU64(request, tag::Generation)});
}

Message ControlService::DispatchGame(Client& client, const Message& request)
{
	auto response = Response(request);
	response.minor = client.requests.Minor();
	InspectionValue document;
	switch (static_cast<OpCode>(request.opcode))
	{
	case OpCode::ReadGameObservations:
	{
		const auto count = request.Find(tag::Length) ? RequireRequestU64(request, tag::Length) : 16;
		if (count > 16)
			throw ControlRequestError{ErrorCode::Invalid, "Game observation page exceeds 16 records"};
		const auto offset =
			request.Find(tag::Offset) ? std::optional{RequireRequestU64(request, tag::Offset)} : std::nullopt;
		const auto page = m_runtime.Game().Observations(offset, static_cast<std::size_t>(count));
		std::vector<InspectionValue> records;
		static constexpr std::array kinds{"registered", "telemetry", "unregistered", "closed"};
		for (const auto& observation : page.observations)
		{
			auto time = InspectionValue::Null();
			if (observation.time)
				time = InspectionValue::Object(
					{{"domain", InspectionValue::Unsigned(static_cast<std::uint32_t>(observation.time->domain))},
					 {"nanoseconds", InspectionValue::Signed(observation.time->nanoseconds)},
					 {"resolution", InspectionValue::Signed(observation.time->resolutionNanoseconds)}});
			records.push_back(InspectionValue::Object(
				{{"sequence", InspectionValue::Unsigned(observation.sequence)},
				 {"game_id", InspectionValue::Unsigned(observation.game)},
				 {"revision", InspectionValue::Unsigned(observation.revision)},
				 {"capabilities", InspectionValue::Unsigned(observation.capabilities)},
				 {"kind", InspectionValue::Text(kinds.at(static_cast<std::size_t>(observation.kind)))},
				 {"observed_time", std::move(time)},
				 {"payload", InspectionValue::Text(observation.payload)}}));
		}
		document = InspectionValue::Object(
			{{"schema", InspectionValue::Unsigned(1)},
			 {"floor", InspectionValue::Unsigned(page.floor)},
			 {"offset", InspectionValue::Unsigned(page.offset)},
			 {"next", InspectionValue::Unsigned(page.next)},
			 {"total", InspectionValue::Unsigned(page.total)},
			 {"gap", InspectionValue::Boolean(page.gap)},
			 {"failed", InspectionValue::Boolean(page.failed)},
			 {"state", InspectionValue::Object({{"game_id", InspectionValue::Unsigned(page.state.game)},
												{"revision", InspectionValue::Unsigned(page.state.revision)},
												{"capabilities", InspectionValue::Unsigned(page.state.capabilities)},
												{"telemetry", InspectionValue::Text(page.state.telemetry)}})},
			 {"observations", InspectionValue::Array(std::move(records))}});
		break;
	}
	case OpCode::GameStatus:
	{
		const auto game = m_runtime.Game().Snapshot();
		response.fields.push_back(Field::U64(tag::GameId, game.game));
		document = InspectionValue::Object({{"game_id", InspectionValue::Unsigned(game.game)},
											{"capabilities", InspectionValue::Unsigned(game.capabilities)},
											{"revision", InspectionValue::Unsigned(game.revision)},
											{"telemetry", InspectionValue::Text(game.telemetry)},
											{"queued", InspectionValue::Unsigned(game.queued)},
											{"delivered", InspectionValue::Unsigned(game.delivered)}});
		break;
	}
	case OpCode::SubmitGame:
	case OpCode::GameOperation:
	{
		std::optional<GameCommandRecord> operation;
		const auto game = RequireRequestU64(request, tag::GameId);
		if (request.opcode == static_cast<std::uint16_t>(OpCode::SubmitGame))
		{
			const auto* payload = request.Find(tag::Detail);
			if (!payload || !payload->AsText())
				throw ControlRequestError{ErrorCode::Invalid, "Game command requires a text payload"};
			operation =
				m_runtime.Game().Submit(game, client.identity.value, RequireRequestU64(request, tag::CommandId),
										RequireRequestU32(request, tag::GameKind), std::string{*payload->AsText()});
		}
		else
			operation =
				m_runtime.Game().Read(game, client.identity.value, RequireRequestU64(request, tag::OperationId));
		if (!operation)
			throw ControlRequestError{ErrorCode::Conflict,
									  "Game command unavailable, identity changed or bounded capacity exhausted"};
		static constexpr std::array s_phases{"queued", "delivered", "complete", "failed", "cancelled", "interrupted"};
		response.fields.push_back(Field::U64(tag::GameId, game));
		response.fields.push_back(Field::U64(tag::OperationId, operation->id));
		response.fields.push_back(Field::Text(tag::Phase, s_phases.at(static_cast<std::size_t>(operation->phase))));
		document =
			InspectionValue::Object({{"command_id", InspectionValue::Unsigned(operation->clientId)},
									 {"kind", InspectionValue::Unsigned(operation->kind)},
									 {"payload", InspectionValue::Text(operation->payload)},
									 {"result", InspectionValue::Text(operation->result)},
									 {"source_disconnected", InspectionValue::Boolean(operation->sourceDisconnected)}});
		break;
	}
	default:
		throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported game operation"};
	}
	response.fields.push_back(Field::Bytes(tag::Document, document.Encode(), true));
	return response;
}
Message ControlService::DispatchWindow(Client& client, const Message& request, HeldReaders& held,
									   Publication& publication, Message response)
{
	const auto opcode = static_cast<OpCode>(request.opcode);
	const auto id = RequireRequestU64(request, tag::ImageWindowId);
	if (opcode == OpCode::ReleaseImageWindow)
	{
		held.window = client.windows.Release(id);
		response.fields.push_back(Field::Bool(tag::Released, held.window.window != nullptr));
		return response;
	}
	held.window = client.windows.Read(id);
	if (opcode == OpCode::ReadImageWindow)
	{
		response.fields.push_back(
			Field::Bytes(tag::Document, InspectImageWindow(held.window.window->Snapshot()).Encode(), true));
		return response;
	}
	held.historicalImage = held.window.window->Find({RequireRequestU64(request, tag::FrameId)});
	const auto& image = held.historicalImage;
	if (!image.metadata)
		throw ControlRequestError{ErrorCode::NotFound, "Frame is not in this image window"};
	if (!image.output)
		throw ControlRequestError{ErrorCode::UnsupportedCommand, "Window frame has no retained GPU image"};
	const auto completion = image.output->PollCompletion();
	if (completion != CompletionStatus::Complete)
		throw ControlRequestError{completion == CompletionStatus::Pending ? ErrorCode::Busy : ErrorCode::Internal,
								  "Window image is not GPU-ready"};
	client.images.Retain(response, {held.window.session, image.metadata->key, {}, HistoryTarget(image.view)},
						 image.output, *m_images, client.nativeHandles, publication.images);
	return response;
}
Message ControlService::DispatchFrame(Client& client, const Message& request, const ControlSession& binding,
									  HeldReaders& held, Publication& publication, Message response)
{
	const auto& session = binding.session;
	switch (static_cast<OpCode>(request.opcode))
	{
	case OpCode::BeginImageWindow:
	{
		if (!session->SupportsImageHistory())
			throw ControlRequestError{ErrorCode::UnsupportedCommand, "GPU image windows are unavailable"};
		held.startedWindow = session->ImageHistory().BeginWindow(HistoryView(request, *session));
		held.window = {binding.id, held.startedWindow.window};
		if (!held.window.window)
			throw ControlRequestError{ErrorCode::Busy, "History is empty or another window is collecting"};
		try
		{
			const auto snapshot = held.window.window->Snapshot();
			if (snapshot.generation.value != RequireRequestU64(request, tag::Generation))
				throw ControlRequestError{ErrorCode::StaleGeneration, "Image window belongs to another run"};
			const auto id = client.windows.Store(binding.id, held.window.window);
			publication.window = id;
			response.fields.push_back(Field::U64(tag::ImageWindowId, id));
			response.fields.push_back(Field::Bytes(tag::Document, InspectImageWindow(snapshot).Encode(), true));
			return response;
		}
		catch (...)
		{
			held.window.window->Cancel();
			throw;
		}
	}
	case OpCode::ReadImageHistory:
	{
		if (!session->SupportsImageHistory())
			throw ControlRequestError{ErrorCode::UnsupportedCommand, "GPU image history is unavailable"};
		const auto count =
			request.Find(tag::Length) ? RequireRequestU64(request, tag::Length) : FrameImageHistory::Capacity;
		if (!count || count > FrameImageHistory::Capacity)
			throw ControlRequestError{ErrorCode::Invalid, "Image history count must be in [1,10]"};
		held.history =
			session->ImageHistory().Snapshot(static_cast<std::size_t>(count), HistoryView(request, *session));
		if (held.history.generation.value != RequireRequestU64(request, tag::Generation))
			throw ControlRequestError{ErrorCode::StaleGeneration, "Image history belongs to another run"};
		response.fields.push_back(Field::Bytes(tag::Document, InspectImageHistory(held.history).Encode(), true));
		return response;
	}
	case OpCode::RetainHistoricalImage:
	{
		if (!m_images || !session->SupportsImageHistory())
			throw ControlRequestError{ErrorCode::UnsupportedCommand, "GPU image history is unavailable"};
		const FrameKey frame{{RequireRequestU64(request, tag::Generation)}, {RequireRequestU64(request, tag::FrameId)}};
		held.historicalImage = session->ImageHistory().Find(frame, HistoryView(request, *session));
		const auto& image = held.historicalImage;
		if (!image.metadata)
			throw ControlRequestError{ErrorCode::NotFound, "Historical image is absent or evicted"};
		response.fields.push_back(Field::Bool(tag::ImageAvailable, image.output != nullptr));
		if (!image.output)
		{
			if ((image.view == ImageHistoryView::Quads && !image.quadVisible) ||
				(image.metadata->submission && !image.metadata->submission->layerCount))
			{
				response.fields.push_back(EncodeFrameKey(tag::Frame, frame));
				return response;
			}
			throw ControlRequestError{ErrorCode::UnsupportedCommand, "This frame has no retained historical image"};
		}
		const auto completion = image.output->PollCompletion();
		if (completion != CompletionStatus::Complete)
			throw ControlRequestError{completion == CompletionStatus::Pending ? ErrorCode::Busy : ErrorCode::Internal,
									  "Historical image is not GPU-ready"};
		client.images.Retain(response, {binding.id, frame, {}, HistoryTarget(image.view)}, image.output, *m_images,
							 client.nativeHandles, publication.images, false);
		return response;
	}
	case OpCode::ReadFrameHistory:
	{
		const auto count = request.Find(tag::Length) ? RequireRequestU64(request, tag::Length) : 128;
		const auto after = request.Find(tag::Offset) ? RequireRequestU64(request, tag::Offset) : 0;
		if (!count || count > 128)
			throw ControlRequestError{ErrorCode::Invalid, "Frame history count must be in [1,128]"};
		const auto history = session->History().Snapshot();
		if (history.generation.value != RequireRequestU64(request, tag::Generation))
			throw ControlRequestError{ErrorCode::StaleGeneration, "Frame history belongs to another run"};
		EncodeFrameHistory(response, history, after, static_cast<std::size_t>(count), client.requests.Minor() >= 10);
		return response;
	}
	case OpCode::ReadProjectionHistory:
	{
		const auto count = request.Find(tag::Length) ? RequireRequestU64(request, tag::Length) : 64;
		if (!count || count > 64)
			throw ControlRequestError{ErrorCode::Invalid, "Projection history count must be in [1,64]"};
		const auto history = session->History().Snapshot();
		if (history.generation.value != RequireRequestU64(request, tag::Generation))
			throw ControlRequestError{ErrorCode::StaleGeneration, "Projection history belongs to another run"};
		const auto document = InspectProjectionHistory(history, static_cast<std::size_t>(count));
		response.fields.push_back(Field::Bytes(tag::Document, document.Encode(), true));
		return response;
	}
	case OpCode::InspectFrame:
	{
		auto capture = session->Captures().Read({RequireRequestU64(request, tag::CaptureId)});
		if (!capture)
			throw ControlRequestError{ErrorCode::NotFound, "Snapshot is absent or released"};
		held.capture = std::move(capture);
		const auto& value = *held.capture;
		if (!value.metadata || !value.metadata->submission)
			throw ControlRequestError{ErrorCode::Busy, "Submitted CPU metadata is not available yet"};
		const bool existing = client.inspections.Find(binding.id, value.metadata->key) != nullptr;
		const auto inspection = InspectFrame(client.inspections, binding.id, *value.metadata);
		if (!existing)
			publication.inspection = inspection->id;
		response.fields.push_back(Field::U64(tag::CaptureId, value.id.value));
		response.fields.push_back(Field::U64(tag::InspectionId, inspection->id));
		response.fields.push_back(EncodeFrameKey(tag::Frame, inspection->frame));
		response.fields.push_back(Field::Bytes(tag::Document, inspection->document, true));
		return response;
	}
	case OpCode::StartVideoExport:
	{
		const auto first = RequireRequestU64(request, tag::FirstFrame),
				   last = RequireRequestU64(request, tag::LastFrame);
		const SessionGeneration generation{RequireRequestU64(request, tag::Generation)};
		std::vector<PreviewClipFrame> frames;
		try
		{
			frames = session->PreviewHistory().RetainRange({generation, {first}}, {generation, {last}});
		}
		catch (const std::runtime_error& error)
		{
			throw ControlRequestError{ErrorCode::NotFound, error.what()};
		}
		client.images.StartVideo(response, {binding.id, {generation, {first}}, {}, {CaptureKind::VirtualDisplay}},
								 std::move(frames), publication.images);
		return response;
	}
	case OpCode::RetainPreview:
	{
		if (request.Find(tag::FrameId))
		{
			const FrameKey key{{RequireRequestU64(request, tag::Generation)},
							   {RequireRequestU64(request, tag::FrameId)}};
			auto preview = session->PreviewHistory().Find(key);
			if (preview.pending)
			{
				const auto error = session->PreviewHistory().Error();
				throw ControlRequestError{error.empty() ? ErrorCode::Busy : ErrorCode::Internal,
										  error.empty()
											  ? "Decoding exact history frame"
											  : "Could not decode this frame. Check history status for details."};
			}
			if (!preview.submitted)
				throw ControlRequestError{ErrorCode::NotFound, "Frame preview is absent or evicted"};
			if (!preview.output && !preview.clear)
				throw ControlRequestError{ErrorCode::NotFound, "Frame preview was evicted or could not be retained"};
			if (preview.tracking)
				response.fields.push_back(
					Field::Bytes(tag::Document, InspectPreviewTracking(*preview.tracking).Encode()));
			held.historicalImage.output = std::move(preview.output);
			response.fields.push_back(Field::Bool(tag::ImageAvailable, held.historicalImage.output != nullptr));
			if (!held.historicalImage.output)
			{
				response.fields.push_back(EncodeFrameKey(tag::Frame, key));
				return response;
			}
			const auto completion = held.historicalImage.output->PollCompletion();
			if (completion != CompletionStatus::Complete)
				throw ControlRequestError{completion == CompletionStatus::Pending ? ErrorCode::Busy
																				  : ErrorCode::Internal,
										  "Frame preview is not GPU-ready"};
			client.images.Retain(response, {binding.id, key, {}, {CaptureKind::VirtualDisplay}},
								 held.historicalImage.output, *m_images, client.nativeHandles, publication.images,
								 false);
			return response;
		}
		held.display = session->Display().Snapshot();
		const auto& display = *held.display;
		if (display.generation.value != RequireRequestU64(request, tag::Generation))
			throw ControlRequestError{ErrorCode::StaleGeneration, "Display generation differs from the requested run"};
		if (!display.latest)
			throw ControlRequestError{ErrorCode::Busy, "Virtual display has not selected an image or clear yet"};
		const auto& selected = *display.latest;
		response = Response(request);
		response.minor = client.requests.Minor();
		response.fields.push_back(Field::U64(tag::Session, binding.id.value));
		response.fields.push_back(Field::U64(tag::Generation, display.generation.value));
		response.fields.push_back(EncodeDisplayTick(tag::Display, selected));
		// Age belongs to the original game prediction, not a newer reprojected image.
		std::int64_t sourceAge = -1;
		if (selected.metadata)
		{
			const auto mapping = session->Scheduler().NativeClockMapping();
			const auto& prediction = selected.metadata->prediction;
			if (mapping && mapping->generation == display.generation && mapping->revision == prediction.clockRevision &&
				prediction.displayTime.value.count() > 0)
				sourceAge = mapping->runtimeAfter > prediction.displayTime
								? (mapping->runtimeAfter.value - prediction.displayTime.value).count()
								: 0;
		}
		response.fields.push_back(Field::I64(tag::SourceAge, sourceAge));
		response.fields.push_back(Field::Bool(tag::ImageAvailable, selected.output != nullptr));
		if (!selected.output)
			return response;
		if (!selected.source || selected.source->generation != display.generation ||
			selected.output->PollCompletion() != CompletionStatus::Complete)
			throw ControlRequestError{ErrorCode::Busy, "Selected display image is not complete"};
		if (const auto tracking = session->PreviewHistory().Tracking(*selected.source))
			response.fields.push_back(Field::Bytes(tag::Document, InspectPreviewTracking(*tracking).Encode()));
		const ImageSource source{binding.id, *selected.source, {}, {CaptureKind::VirtualDisplay}, selected.imageTick};
		client.images.Retain(response, source, selected.output, *m_images, client.nativeHandles, publication.images,
							 false);
		return response;
	}
	case OpCode::ReleaseSequenceCapture:
	{
		const CaptureId id{RequireRequestU64(request, tag::CaptureId)};
		const SessionGeneration generation{RequireRequestU64(request, tag::Generation)};
		bool released = false;
		const auto release = [&]
		{
			held.capture = session->Captures().Read(id);
			if (!held.capture)
				return;
			if (held.capture->generation != generation)
				throw ControlRequestError{ErrorCode::StaleGeneration, "Sequence capture belongs to another run"};
			const bool owned = ApplicationOwnedSequence(request)
				? held.capture->metadata && held.capture->metadata->sequenceCapture &&
					held.capture->metadata->sequenceCapture->request.owner == ApplicationAutomationSource &&
					held.capture->metadata->sequenceCapture->reservation.id == id
				: client.simulation->OwnsSequenceCapture(*held.capture);
			if (!owned)
				throw ControlRequestError{ErrorCode::Conflict, "Sequence capture belongs to another source"};
			released = session->Captures().Release(id);
		};
		if (!session->WithCommandGeneration(generation, release, false))
			throw ControlRequestError{ErrorCode::StaleGeneration, "Sequence generation changed before release"};
		response.fields.push_back(Field::Bool(tag::Released, released));
		return response;
	}
	case OpCode::RetainImage:
	{
		auto capture = session->Captures().Read({RequireRequestU64(request, tag::CaptureId)});
		if (!capture)
			throw ControlRequestError{ErrorCode::NotFound, "Snapshot is absent or released"};
		held.capture = std::move(capture);
		if (request.Find(tag::Target))
		{
			const auto target = DecodeCaptureTarget(request);
			if (!session->SupportsCapture(target))
				throw ControlRequestError{ErrorCode::UnsupportedCommand, "Image target unsupported on this session"};
			if (!held.capture->frame)
				throw ControlRequestError{ErrorCode::Busy, "Capture has no submitted frame yet"};
			const auto selected = session->Captures().Find(*held.capture->frame, target);
			if (selected)
				held.selectedCapture = session->Captures().Read(*selected);
			if (!held.selectedCapture)
				throw ControlRequestError{ErrorCode::NotFound, "Image target was not retained for this frame"};
		}
		const auto& value = held.selectedCapture ? *held.selectedCapture : *held.capture;
		if (!value.frame || value.generation.value != RequireRequestU64(request, tag::Generation))
			throw ControlRequestError{ErrorCode::StaleGeneration,
									  "Capture generation differs from the requested session run"};
		const bool ready = value.output && (value.phase == SnapshotPhase::Ready ||
											(value.phase == SnapshotPhase::GpuPending &&
											 value.output->PollCompletion() == CompletionStatus::Complete));
		if (!ready)
			throw ControlRequestError{value.phase == SnapshotPhase::GpuPending ? ErrorCode::Busy
																			   : ErrorCode::UnsupportedCommand,
									  "Capture has no ready GPU image"};
		const ImageSource source{binding.id, *value.frame, value.id, value.target};
		response = Response(request);
		response.minor = client.requests.Minor();
		client.images.Retain(response, source, value.output, *m_images, client.nativeHandles, publication.images);
		return response;
	}
	case OpCode::Capture:
	{
		auto capture = session->Captures().Read({RequireRequestU64(request, tag::CaptureId)});
		if (!capture)
			throw ControlRequestError{ErrorCode::NotFound, "Snapshot is absent or released"};
		held.capture = std::move(capture);
		const auto& value = *held.capture;
		EncodeCaptureMetadata(response, value, client.requests.Minor() >= 10);
		return response;
	}
	default:
		throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported frame inspection operation"};
	}
}
Message ControlService::DispatchDebugger(Client& client, const Message& request, const ControlSession& binding,
										 HeldReaders& held, Message response)
{
	const auto& session = binding.session;
	if (static_cast<OpCode>(request.opcode) == OpCode::ConfigurePreviews)
	{
		const auto bytes = RequireRequestU64(request, tag::PreviewBudget);
		if (bytes < (std::uint64_t{FramePreviewHistory::MinMiB} << 20) ||
			bytes > (std::uint64_t{FramePreviewHistory::MaxMiB} << 20) || bytes % FramePreviewHistory::SlotBytes)
			throw ControlRequestError{ErrorCode::Invalid, "History budget must be 64–16384 MiB in multiples of 4 MiB"};
		if (request.Find(tag::PreviewRamBudget))
		{
			const auto ram = RequireRequestU64(request, tag::PreviewRamBudget);
			if (ram < (std::uint64_t{FramePreviewHistory::MinMiB} << 20) ||
				ram > (std::uint64_t{FramePreviewHistory::MaxMiB} << 20) || ram % FramePreviewHistory::SlotBytes)
				throw ControlRequestError{ErrorCode::Invalid,
										  "RAM history budget must be 64–16384 MiB in multiples of 4 MiB"};
			session->PreviewHistory().ConfigureRam(ram);
		}
		session->PreviewHistory().Configure(bytes);
		response.fields.push_back(Field::U64(tag::PreviewBudget, session->PreviewHistory().Limit()));
		response.fields.push_back(Field::U64(tag::PreviewRamBudget, session->PreviewHistory().RamLimit()));
		response.fields.push_back(Field::U64(tag::PreviewRamBytes, session->PreviewHistory().RamBytes()));
		response.fields.push_back(Field::Text(tag::PreviewError, session->PreviewHistory().Error()));
		return response;
	}

	auto controller = m_runtime.Debugger(binding.handle);
	if (!controller)
		throw ControlRequestError{ErrorCode::Closed, "Session debugger closed"};
	held.controller = controller;
	switch (static_cast<OpCode>(request.opcode))
	{
	case OpCode::Status:
	{
		const auto status = controller->Snapshot();
		const auto state = session->Snapshot();
		held.display = session->Display().Snapshot();
		const auto& display = *held.display;
		auto capabilities = ServiceCapabilities(client.requests.Minor());
		if (m_images && client.requests.Minor() >= 2)
		{
			const auto native = m_images->Capabilities(binding.handle);
			capabilities |= ImageCapabilitiesMask(native, client.nativeHandles);
			if (native.rotationalReprojection)
				capabilities |= static_cast<std::uint64_t>(Capability::RotationalReprojection);
			if (client.requests.Minor() >= 3 && (native.gpuPreview || native.imageExport))
				capabilities |= static_cast<std::uint64_t>(Capability::LivePreview) |
								static_cast<std::uint64_t>(Capability::ContinuousPreviews) |
								static_cast<std::uint64_t>(Capability::ConfigurablePreviews);
		}
		if (status.composedEyes)
			capabilities |= static_cast<std::uint64_t>(Capability::ComposedCapture) |
							static_cast<std::uint64_t>(Capability::ImageRecording);
		if (session->SupportsImageHistory())
			capabilities |= static_cast<std::uint64_t>(Capability::ImageHistory) |
							static_cast<std::uint64_t>(Capability::ImageWindow);
		if (session->SupportsImageHistory() && status.filteredComposition)
			capabilities |= static_cast<std::uint64_t>(Capability::QuadImageHistory);
		if (status.filteredComposition)
			capabilities |= static_cast<std::uint64_t>(Capability::FilteredComposition);
		if (status.depthViews)
			capabilities |= static_cast<std::uint64_t>(Capability::DepthCapture);
		if (status.layerViews)
			capabilities |= static_cast<std::uint64_t>(Capability::LayerViewCapture);
		EncodeDebuggerStatus(response, status, state, display, capabilities);
		std::vector<Field> previews;
		auto frames = session->PreviewHistory().Frames(state.generation, 65536);
		std::vector<std::byte> ids;
		ids.reserve(frames.size() * sizeof(std::uint64_t));
		for (std::size_t index = 0; index < frames.size(); ++index)
		{
			const auto id = frames[index].frame.value;
			for (unsigned shift = 0; shift < 64; shift += 8)
				ids.push_back(static_cast<std::byte>((id >> shift) & 255));
			if (frames.size() - index <= MaxFields)
				previews.push_back(Field::U64(static_cast<std::uint16_t>(previews.size() + 1), id));
		}
		response.fields.push_back(Field::Bytes(tag::PreviewFrameIds, ids));
		response.fields.push_back(Nested(tag::PreviewFrames, std::move(previews)));
		response.fields.push_back(Field::U64(tag::PreviewBytes, session->PreviewHistory().Bytes()));
		response.fields.push_back(Field::U64(tag::PreviewBudget, session->PreviewHistory().Limit()));
		response.fields.push_back(Field::U64(tag::PreviewRamBudget, session->PreviewHistory().RamLimit()));
		response.fields.push_back(Field::U64(tag::PreviewRamBytes, session->PreviewHistory().RamBytes()));
		response.fields.push_back(Field::Text(tag::PreviewError, session->PreviewHistory().Error()));
		return response;
	}
	case OpCode::Acquire:
	{
		const auto lease = client.leases.Acquire(binding.id, controller, *session,
												 {RequireRequestU64(request, tag::Generation)}, held.leases);
		response.fields.push_back(Field::U64(tag::Lease, lease.id.value));
		response.fields.push_back(
			Field::I64(tag::LeaseRemaining, (lease.expires - session->ClockSource()->Now()).count()));
		return response;
	}
	case OpCode::Renew:
		client.leases.Renew(binding.id, {RequireRequestU64(request, tag::Lease)}, *session,
							{RequireRequestU64(request, tag::Generation)});
		return response;
	case OpCode::Relinquish:
		client.leases.Relinquish(binding.id, {RequireRequestU64(request, tag::Lease)}, *session,
								 {RequireRequestU64(request, tag::Generation)}, held.leases);
		return response;
	case OpCode::Submit:
	{
		const auto command = DecodeDebugRequest(request);
		EncodeDebugAdmission(response,
							 client.leases.Submit(binding.id, {RequireRequestU64(request, tag::Lease)}, command));
		return response;
	}
	case OpCode::Operation:
	{
		auto operation = controller->Read({RequireRequestU64(request, tag::OperationId)});
		if (!operation)
			throw ControlRequestError{ErrorCode::NotFound, "Operation is absent or acknowledged"};
		held.operation = std::move(operation);
		const auto& value = *held.operation;
		EncodeDebugOperation(response, value);
		return response;
	}
	case OpCode::Acknowledge:
		client.leases.Acknowledge(binding.id, {RequireRequestU64(request, tag::Lease)},
								  {RequireRequestU64(request, tag::CommandId)}, *session,
								  {RequireRequestU64(request, tag::Generation)}, held.leases);
		return response;

	default:
		throw ControlRequestError{ErrorCode::UnsupportedCommand, "Unsupported operation"};
	}
}
Message ControlService::Dispatch(Client& client, const Message& request, HeldReaders& held, Publication& publication,
								 const ControlPacket& incoming, ControlPacket& outgoing)
{
	const auto& schema = ValidateControlRequest(incoming, client.nativeHandles);
	const auto opcode = schema.opcode;
	auto response = Response(request);
	response.fields.reserve(32);
	if (opcode == OpCode::Hello)
	{
		if (client.requests.Negotiated())
			throw ControlRequestError{ErrorCode::Conflict, "Connection already negotiated"};
		if (const auto* name = request.Find(tag::ClientName); name && (!name->AsText() || name->value.size() > 256))
			throw ControlRequestError{ErrorCode::Invalid, "Invalid client name"};
		const auto* required = request.Find(tag::RequiredCapabilities);
		const auto* optional = request.Find(tag::OptionalCapabilities);
		if ((required && !required->AsU64()) || (optional && !optional->AsU64()))
			throw ControlRequestError{ErrorCode::Invalid, "Invalid capability mask"};
		const auto negotiatedMinor = std::min(request.minor, ProtocolMinor);
		const auto capabilities = AdvertisedCapabilities(client, negotiatedMinor);
		if (required && (*required->AsU64() & ~capabilities))
			throw ControlRequestError{ErrorCode::UnsupportedCommand, "Required control capability unavailable"};
		response.minor = negotiatedMinor;
		response.fields.push_back(Field::U32(tag::Minor, negotiatedMinor));
		response.fields.push_back(Field::U64(tag::Capabilities, capabilities));
		response.fields.push_back(Field::U32(tag::PayloadLimit, MaxPayload));
		return response;
	}
	if (!client.requests.Negotiated())
		throw ControlRequestError{ErrorCode::NotNegotiated, "Hello is required"};
	response.minor = client.requests.Minor();
	if (client.requests.Minor() < schema.minimumMinor)
		throw ControlRequestError{ErrorCode::UnsupportedCommand, "Operation unavailable in negotiated protocol minor"};
	if (schema.domain == RequestDomain::Images)
	{
		if (!m_images)
			throw ControlRequestError{ErrorCode::UnsupportedCommand, "Image inspection service unavailable"};
		if (opcode == OpCode::ReadImageWindow || opcode == OpCode::ReleaseImageWindow ||
			opcode == OpCode::RetainWindowImage)
		{
			return DispatchWindow(client, request, held, publication, std::move(response));
		}
		if (opcode != OpCode::RetainImage && opcode != OpCode::RetainPreview && opcode != OpCode::StartVideoExport)
			return client.images.Dispatch(incoming, outgoing, client.requests.Minor(), client.nativeHandles,
										  publication.images, held.images);
	}
	if (opcode == OpCode::Sessions)
	{
		std::vector<Field> entries;
		m_sessions.Discover(held.sessions);
		client.leases.Prune(held.sessions.Ids(), held.leases);
		for (const auto& binding : held.sessions.Sessions())
		{
			const auto state = binding.session->Snapshot();
			entries.push_back(
				Nested(static_cast<std::uint16_t>(entries.size() + 1),
					   {Field::U64(tag::Session, binding.id.value), Field::U64(tag::Generation, state.generation.value),
						Field::Bool(tag::Running, state.running), Field::Bool(tag::Closed, state.closed)}));
		}
		response.fields.push_back(Nested(tag::Sessions, std::move(entries)));
		return response;
	}
	// Connection-owned immutable metadata remains readable after its session
	// ends. No native or GPU resource ownership is retained by this catalog.
	if (opcode == OpCode::FeatureRevision)
	{
		auto revision = client.inspections.ReadRevision(RequireRequestU64(request, tag::RevisionId));
		if (!revision)
			throw ControlRequestError{ErrorCode::NotFound, "Revision is absent or no longer retained"};
		response.fields.push_back(Field::U64(tag::RevisionId, revision->id));
		response.fields.push_back(Field::Text(tag::RevisionKind, revision->document.kind));
		response.fields.push_back(Field::Bytes(tag::Document, revision->document.bytes, true));
		return response;
	}
	if (opcode == OpCode::ReleaseInspection)
	{
		const auto id = RequireRequestU64(request, tag::InspectionId);
		response.fields.push_back(Field::U64(tag::InspectionId, id));
		response.fields.push_back(Field::Bool(tag::Released, client.inspections.Release(id)));
		return response;
	}
	if (schema.domain == RequestDomain::Game)
	{
		std::optional<FrameNativeClockMapping> mapping;
		if (request.Find(tag::Session) || request.Find(tag::Generation))
		{
			const auto& binding = Resolve(client, request, held);
			if (request.opcode == static_cast<std::uint16_t>(OpCode::ReadGameObservations) &&
				!binding.session->WithCommandGeneration({RequireRequestU64(request, tag::Generation)},
														[&]
														{
															mapping = binding.session->Scheduler().NativeClockMapping();
														}))
				throw ControlRequestError{ErrorCode::StaleGeneration, "Session changed before SDK clock observation"};
		}
		auto result = DispatchGame(client, request);
		if (request.opcode == static_cast<std::uint16_t>(OpCode::ReadGameObservations))
		{
			if (request.Find(tag::Session))
			{
				result.fields.push_back(Field::U64(tag::Session, RequireRequestU64(request, tag::Session)));
				result.fields.push_back(Field::U64(tag::Generation, RequireRequestU64(request, tag::Generation)));
			}
			if (mapping)
			{
				result.fields.push_back(Field::U32(tag::ClockDomain, static_cast<std::uint32_t>(mapping->domain)));
				result.fields.push_back(Field::U64(tag::ClockRevision, mapping->revision));
				result.fields.push_back(Field::I64(tag::NativeTime, mapping->nativeNanoseconds));
				result.fields.push_back(Field::I64(tag::RuntimeTimeBefore, mapping->runtimeBefore.value.count()));
				result.fields.push_back(Field::I64(tag::RuntimeTimeAfter, mapping->runtimeAfter.value.count()));
				result.fields.push_back(Field::I64(tag::ClockResolution, mapping->resolutionNanoseconds));
			}
		}
		return result;
	}
	const auto& binding = Resolve(client, request, held);
	const auto& session = binding.session;
	if (schema.domain == RequestDomain::Simulation)
		return client.simulation->Dispatch(request, binding, std::move(response), held.sequenceCaptures);
	response.fields.push_back(Field::U64(tag::Session, binding.id.value));
	response.fields.push_back(Field::U64(tag::Generation, session->Snapshot().generation.value));
	if (schema.domain == RequestDomain::Images || schema.domain == RequestDomain::Inspection)
		return DispatchFrame(client, request, binding, held, publication, std::move(response));
	return DispatchDebugger(client, request, binding, held, std::move(response));
}
ControlService::ControlService(Runtime& runtime, std::size_t capacity, std::shared_ptr<ImageInspectionProvider> images)
	: m_runtime(runtime), m_images(std::move(images)), m_capacity(capacity), m_sessions(runtime)
{
	if (!capacity || capacity > 64)
		throw std::invalid_argument("Invalid control connection capacity");
	// Registration cannot allocate after transferring the client owner while
	// the registry mutex is held: its destructor may retire runtime sources.
	m_clients.reserve(capacity);
}
ControlService::~ControlService()
{
	Close();
}
std::optional<ConnectionId> ControlService::Connect(bool nativeHandles)
{
	auto client = std::make_shared<Client>(m_runtime, nativeHandles);
	std::lock_guard lock(m_mutex);
	if (m_closed || m_clients.size() >= m_capacity || m_nextConnection == std::numeric_limits<std::uint64_t>::max())
		return {};
	const ConnectionId id{m_nextConnection++};
	client->identity = id;
	client->simulation = std::make_unique<SimulationConnection>(id, m_runtime.Game());
	m_clients.push_back({id, std::move(client)});
	return id;
}
void ControlService::Disconnect(ConnectionId connection) noexcept
{
	std::shared_ptr<Client> client;
	{
		std::lock_guard lock(m_mutex);
		const auto found = std::find_if(m_clients.begin(), m_clients.end(),
										[&](const auto& entry)
										{
											return entry.first == connection;
										});
		if (found == m_clients.end())
			return;
		client = std::move(found->second);
		m_clients.erase(found);
	}
	client->requests.Close();
	// Cancel game work immediately, then again at settlement if an already
	// admitted request submitted another command while disconnect was pending.
	m_runtime.Game().Disconnect(client->identity.value, true);
}
ControlPacket ControlService::HandlePacket(ConnectionId connection, const ControlPacket& packet)
{
	const auto& request = packet.message;
	std::uint16_t minor = std::min(request.minor, ProtocolMinor);
	const auto failureResponse = [&](ErrorCode error, const char* detail)
	{
		ControlPacket response;
		response.message = Response(request, error, detail);
		response.message.minor = minor;
		response.encoded = std::make_shared<const std::vector<std::byte>>(Encode(response.message));
		return response;
	};
	try
	{
		auto client = Find(connection);
		if (!client)
			return failureResponse(ErrorCode::Closed, "Connection closed");
		HeldReaders held;
		ConnectionRequests::RetiredReplay retiredReplay;
		auto exchange = client->requests.Begin(packet);
		minor = exchange.ResponseMinor();
		switch (exchange.Result())
		{
		case ConnectionRequests::Exchange::Admission::Reject:
			return failureResponse(exchange.Error(), exchange.Detail());
		case ConnectionRequests::Exchange::Admission::Replay:
			return exchange.Replay();
		case ConnectionRequests::Exchange::Admission::Execute:
			break;
		}
		// Failed response/cache construction removes unannounced resources before
		// reopening admission. Actual GPU ownership remains backend-managed.
		Publication publication{*client, held.images, held.retiredWindow};
		ControlPacket response;
		try
		{
			response.message = Dispatch(*client, request, held, publication, packet, response);
		}
		catch (const ControlRequestError& failure)
		{
			response = failureResponse(failure.error, failure.detail);
		}
		catch (const ConnectionImages::RequestFailure& failure)
		{
			response = failureResponse(failure.error, failure.detail);
		}
		minor = client->requests.ResponseMinor(request.minor);
		response.message.minor = minor;
		response.encoded = std::make_shared<const std::vector<std::byte>>(Encode(response.message));
		exchange.Publish(response, retiredReplay);
		if (response.message.fields.front().AsU32() == static_cast<std::uint32_t>(ErrorCode::Ok))
		{
			// Commit negotiation only after its reply is replayable. Admission
			// remains exclusive, including while a disconnect awaits settlement.
			if (request.opcode == static_cast<std::uint16_t>(OpCode::Hello))
				client->requests.Negotiate(response.message.minor);
			publication.Commit();
		}
		return response;
	}
	catch (const ImageInspectionError& error)
	{
		return failureResponse(error.code, error.what());
	}
	catch (const std::bad_alloc&)
	{
		return failureResponse(ErrorCode::Capacity, "Control allocation capacity exhausted");
	}
	catch (const std::length_error&)
	{
		return failureResponse(ErrorCode::Capacity, "Control message capacity exceeded");
	}
	catch (const std::invalid_argument&)
	{
		return failureResponse(ErrorCode::Invalid, "Malformed control request");
	}
	catch (...)
	{
		return failureResponse(ErrorCode::Internal, "Control request failed");
	}
}
void ControlService::Close() noexcept
{
	for (;;)
	{
		ConnectionId id;
		{
			std::lock_guard lock(m_mutex);
			m_closed = true;
			if (m_clients.empty())
				return;
			id = m_clients.back().first;
		}
		Disconnect(id);
	}
}

Message ControlService::Handle(ConnectionId connection, const Message& request)
{
	ControlPacket packet;
	packet.message = request;
	auto response = HandlePacket(connection, packet);
	return std::move(response.message);
}
} // namespace simxr::control
