#include "control/ConnectionImages.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "control/DebugProtocol.h"
#include "control/VideoExport.h"
#include "graphics/SnapshotBudget.h"

namespace simxr::control
{
template<class Resource>
ConnectionImages::ImageTable<Resource>::ImageTable()
{
	m_entries.reserve(SnapshotBudget::Capacity());
}

template<class Resource>
void ConnectionImages::ImageTable<Resource>::CheckCapacity() const
{
	if (m_entries.size() >= SnapshotBudget::Capacity() || m_next == std::numeric_limits<std::uint64_t>::max())
		throw RequestFailure{ErrorCode::Capacity, "Connection image resource capacity exhausted"};
}

template<class Resource>
auto ConnectionImages::ImageTable<Resource>::Find(std::uint64_t id) const -> std::shared_ptr<const ImageEntry<Resource>>
{
	for (const auto& entry : m_entries)
		if (entry->id == id)
			return entry;
	return {};
}

template<class Resource>
auto ConnectionImages::ImageTable<Resource>::Find(const ImageSource& source) const
	-> std::shared_ptr<const ImageEntry<Resource>>
{
	for (const auto& entry : m_entries)
		if (entry->source == source)
			return entry;
	return {};
}

template<class Resource>
auto ConnectionImages::ImageTable<Resource>::Add(std::shared_ptr<Resource> value, ImageSource source,
												 std::uint64_t reader) -> std::shared_ptr<const ImageEntry<Resource>>
{
	CheckCapacity();
	auto entry = std::make_shared<ImageEntry<Resource>>(ImageEntry<Resource>{m_next, reader, source, std::move(value)});
	m_entries.push_back(entry);
	++m_next;
	return entry;
}

template<class Resource>
auto ConnectionImages::ImageTable<Resource>::Release(std::uint64_t id) noexcept
	-> std::shared_ptr<const ImageEntry<Resource>>
{
	for (auto iterator = m_entries.begin(); iterator != m_entries.end(); ++iterator)
		if ((*iterator)->id == id)
		{
			auto entry = std::move(*iterator);
			m_entries.erase(iterator);
			return entry;
		}
	return {};
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

static std::uint64_t U64(const Message& message, std::uint16_t name)
{
	const auto* field = message.Find(name);
	const auto value = field ? field->AsU64() : std::nullopt;
	if (!value)
		throw ConnectionImages::RequestFailure{ErrorCode::Invalid, "Missing or invalid uint64 field"};
	return *value;
}

static std::uint32_t U32(const Message& message, std::uint16_t name, std::optional<std::uint32_t> fallback = {})
{
	const auto* field = message.Find(name);
	if (!field && fallback)
		return *fallback;
	const auto value = field ? field->AsU32() : std::nullopt;
	if (!value)
		throw ConnectionImages::RequestFailure{ErrorCode::Invalid, "Missing or invalid uint32 field"};
	return *value;
}

static Field Nested(std::uint16_t name, std::vector<Field> fields)
{
	return Field::Bytes(name, EncodeFields(fields));
}

static ImageRegion ReadRegion(const Field& field)
{
	const auto decoded = DecodeFields(field.value);
	if (decoded.error != DecodeError::None)
		throw ConnectionImages::RequestFailure{ErrorCode::Invalid, "Invalid image region"};
	Message message;
	message.fields = decoded.fields;
	constexpr std::array<std::uint16_t, 6> known{1, 2, 3, 4, 5, 6};
	if (message.ValidateFields(known))
		throw ConnectionImages::RequestFailure{ErrorCode::UnsupportedField, "Unknown image region field"};
	return {U32(message, 1), U32(message, 2), U32(message, 3), U32(message, 4), U32(message, 5), U32(message, 6)};
}

static void ExportSelection(Message& response, const ImageExport& value)
{
	if (const auto layer = value.SourceLayer())
		response.fields.push_back(Field::U32(tag::Layer, *layer));
	if (const auto region = value.Region())
		response.fields.push_back(EncodeImageRegion(*region));
}

static Field Key(std::uint16_t name, FrameKey key)
{
	return Nested(name, {Field::U64(tag::Generation, key.generation.value), Field::U64(tag::FrameId, key.frame.value)});
}

static const char* CompletionName(CompletionStatus status)
{
	switch (status)
	{
	case CompletionStatus::Pending:
		return "pending";
	case CompletionStatus::Complete:
		return "complete";
	case CompletionStatus::Failed:
		return "failed";
	}
	return "failed";
}

static std::uint64_t ImageMask(ImageCapabilities capabilities, bool handles)
{
	return (capabilities.imageExport ? static_cast<std::uint64_t>(Capability::ImageExport) : 0) |
		   (capabilities.gpuPreview && handles ? static_cast<std::uint64_t>(Capability::GpuPreview) : 0);
}

static void Source(Message& response, const ImageSource& source, bool session = true)
{
	if (session)
	{
		response.fields.push_back(Field::U64(tag::Session, source.session.value));
		response.fields.push_back(Field::U64(tag::Generation, source.frame.generation.value));
	}
	response.fields.push_back(Key(tag::Frame, source.frame));
	response.fields.push_back(Field::U64(tag::CaptureId, source.capture.value));
	response.fields.push_back(EncodeCaptureTarget(source.target));
	if (source.imageTick)
		response.fields.push_back(Field::U64(tag::ImageTick, source.imageTick->value));
}

static ImageDeviceIdentity DeviceIdentity(const Message& message)
{
	const auto device = message.Find(tag::DeviceUuid), driver = message.Find(tag::DriverUuid);
	if (!device || !driver || device->value.size() != 16 || driver->value.size() != 16)
		throw ConnectionImages::RequestFailure{ErrorCode::Invalid,
											   "Device and driver identities must each contain sixteen bytes"};
	ImageDeviceIdentity identity;
	std::copy(device->value.begin(), device->value.end(), identity.device.begin());
	std::copy(driver->value.begin(), driver->value.end(), identity.driver.begin());
	return identity;
}

ConnectionImages::ConnectionImages() = default;
ConnectionImages::~ConnectionImages() = default;

ConnectionImages::Publication::Publication(ConnectionImages& owner, RetiredResource& retired)
	: m_owner(owner), m_retired(retired)
{}

ConnectionImages::Publication::~Publication()
{
	if (m_kind && !m_committed)
		m_retired.m_value = m_owner.Release(*m_kind, m_id);
}

void ConnectionImages::Publication::Track(ResourceKind kind, std::uint64_t id) noexcept
{
	m_kind = kind;
	m_id = id;
}

void ConnectionImages::Publication::Commit() noexcept
{
	m_committed = true;
}

std::shared_ptr<const void> ConnectionImages::Release(ResourceKind kind, std::uint64_t id) noexcept
{
	switch (kind)
	{
	case ResourceKind::Reader:
		return m_images.Release(id);
	case ResourceKind::Delivery:
		return m_deliveries.Release(id);
	case ResourceKind::Export:
		return m_exports.Release(id);
	}
	return {};
}

void ConnectionImages::Retain(Message& response, ImageSource source, std::shared_ptr<const FrameOutput> output,
							  const ImageInspectionProvider& provider, bool nativeHandles, Publication& publication,
							  bool includeSession)
{
	auto entry = m_images.Find(source);
	if (!entry)
	{
		m_images.CheckCapacity();
		auto reader = provider.Retain(std::move(output));
		if (!reader)
		{
			const auto* detail = source.target.kind == CaptureKind::VirtualDisplay
									 ? "Selected image backend cannot retain preview"
									 : "Capture backend cannot retain this image";
			throw RequestFailure{ErrorCode::UnsupportedCommand, detail};
		}
		// Entry allocation can fail before publication has a table ID to detach.
		publication.m_retired.m_value = reader;
		entry = m_images.Add(std::move(reader), source);
		publication.Track(ResourceKind::Reader, entry->id);
	}
	Source(response, source, includeSession);
	response.fields.push_back(Field::U64(tag::ImageReaderId, entry->id));
	response.fields.push_back(EncodeImageDescription(entry->value->Description()));
	response.fields.push_back(Field::U64(tag::Capabilities, ImageMask(entry->value->Capabilities(), nativeHandles)));
	response.fields.push_back(Field::Text(tag::Phase, CompletionName(entry->value->PollCompletion())));
}

void ConnectionImages::StartVideo(Message& response, ImageSource source, std::vector<PreviewClipFrame> frames,
								  Publication& publication)
{
	m_exports.CheckCapacity();
	auto video = std::make_shared<VideoExport>(std::move(frames));
	publication.m_retired.m_value = video;
	const auto entry = m_exports.Add(std::move(video), source);
	publication.Track(ResourceKind::Export, entry->id);
	Source(response, source, false);
	response.fields.push_back(Field::U64(tag::ExportId, entry->id));
	response.fields.push_back(Field::Text(tag::MediaType, "video/mp4"));
	response.fields.push_back(Field::Text(tag::Phase, "pending"));
}

Message ConnectionImages::Dispatch(const ControlPacket& incoming, ControlPacket& outgoing, std::uint16_t minor,
								   bool nativeHandles, Publication& publication, RetiredResource& retired)
{
	const auto& request = incoming.message;
	auto response = Response(request);
	response.minor = minor;
	response.fields.reserve(32);
	const auto operation = static_cast<OpCode>(request.opcode);
	std::optional<std::pair<std::uint16_t, ResourceKind>> release;
	switch (operation)
	{
	case OpCode::ReleaseImage:
		release = {tag::ImageReaderId, ResourceKind::Reader};
		break;
	case OpCode::ReleaseDelivery:
		release = {tag::DeliveryId, ResourceKind::Delivery};
		break;
	case OpCode::ReleaseExport:
		release = {tag::ExportId, ResourceKind::Export};
		break;
	default:
		break;
	}
	if (release)
	{
		const auto [field, kind] = *release;
		const auto id = U64(request, field);
		// Detach before any response allocation. Even an allocation failure must
		// retire the last native reader after the caller reopens request admission.
		retired.m_value = Release(kind, id);
		response.fields.push_back(Field::U64(field, id));
		response.fields.push_back(Field::Bool(tag::Released, retired.m_value != nullptr));
		return response;
	}
	if (operation == OpCode::ImageInfo || operation == OpCode::ShareImage || operation == OpCode::StartExport)
	{
		const auto image = m_images.Find(U64(request, tag::ImageReaderId));
		if (!image)
			throw RequestFailure{ErrorCode::NotFound, "Image reader is absent or released"};
		Source(response, image->source);
		response.fields.push_back(Field::U64(tag::ImageReaderId, image->id));
		if (operation == OpCode::ImageInfo)
		{
			response.fields.push_back(EncodeImageDescription(image->value->Description()));
			response.fields.push_back(Field::Text(tag::Phase, CompletionName(image->value->PollCompletion())));
			response.fields.push_back(
				Field::U64(tag::Capabilities, ImageMask(image->value->Capabilities(), nativeHandles)));
			return response;
		}
		if (image->value->PollCompletion() != CompletionStatus::Complete)
			throw RequestFailure{ErrorCode::Busy, "Image producer is not complete"};
		if (operation == OpCode::ShareImage)
		{
			if (!nativeHandles || !image->value->Capabilities().gpuPreview)
				throw RequestFailure{ErrorCode::UnsupportedCommand,
									 "GPU delivery is unavailable for this image or transport"};
			const auto completion = request.Find(tag::CompletionKind);
			const auto kind = completion ? completion->AsU32() : std::optional<std::uint32_t>{0};
			if (!kind || *kind != static_cast<std::uint32_t>(image->value->Description().completionKind))
				throw RequestFailure{ErrorCode::Invalid, "Consumer completion kind does not match this image"};
			m_deliveries.CheckCapacity();
			auto delivery = image->value->Share(DeviceIdentity(request), incoming.handles.front());
			if (!delivery)
				throw RequestFailure{ErrorCode::Internal, "Image provider returned no delivery"};
			publication.m_retired.m_value = delivery;
			const auto entry = m_deliveries.Add(std::move(delivery), image->source, image->id);
			publication.Track(ResourceKind::Delivery, entry->id);
			outgoing.handles = entry->value->PrepareHandles();
			if (outgoing.handles.size() != 2 || !outgoing.handles[0] || !outgoing.handles[1])
				throw RequestFailure{ErrorCode::Internal, "GPU delivery requires memory and readiness handles"};
			outgoing.exposure = entry->value;
			const auto key = entry->value->Key();
			response.fields.push_back(Field::U64(tag::DeliveryId, entry->id));
			response.fields.push_back(Field::U64(tag::PoolGeneration, key.pool.value));
			response.fields.push_back(Field::U64(tag::SharedImageId, key.image.value));
			response.fields.push_back(EncodeImageDescription(entry->value->Description(), true));
			response.fields.push_back(Field::U32(tag::HandleCount, 2, true));
			response.fields.push_back(
				Field::Text(tag::ProducerPhase, CompletionName(entry->value->ProducerCompletion())));
			response.fields.push_back(
				Field::Text(tag::ConsumerPhase, CompletionName(entry->value->ConsumerCompletion())));
			return response;
		}
		if (!image->value->Capabilities().imageExport)
			throw RequestFailure{ErrorCode::UnsupportedCommand, "Image export unavailable for this image"};
		m_exports.CheckCapacity();
		const auto layer = request.Find(tag::Layer) ? std::optional{U32(request, tag::Layer)} : std::nullopt;
		const auto* regionField = request.Find(tag::ImageRegion);
		if (regionField && minor < 16)
			throw RequestFailure{ErrorCode::UnsupportedField, "Image region export requires protocol minor 16"};
		auto readback =
			regionField ? image->value->ExportRegion(layer, ReadRegion(*regionField)) : image->value->Export(layer);
		if (!readback)
			throw RequestFailure{ErrorCode::Internal, "Image provider returned no export"};
		publication.m_retired.m_value = readback;
		const auto entry = m_exports.Add(std::move(readback), image->source, image->id);
		publication.Track(ResourceKind::Export, entry->id);
		response.fields.push_back(Field::U64(tag::ExportId, entry->id));
		ExportSelection(response, *entry->value);
		response.fields.push_back(EncodeImageDescription(entry->value->Description()));
		response.fields.push_back(Field::U64(tag::TotalBytes, entry->value->ByteSize()));
		response.fields.push_back(Field::Text(tag::Phase, CompletionName(entry->value->PollCompletion())));
		return response;
	}
	if (operation == OpCode::DeliveryInfo)
	{
		const auto entry = m_deliveries.Find(U64(request, tag::DeliveryId));
		if (!entry)
			throw RequestFailure{ErrorCode::NotFound, "Delivery is absent or released"};
		Source(response, entry->source);
		const auto key = entry->value->Key();
		response.fields.push_back(Field::U64(tag::ImageReaderId, entry->reader));
		response.fields.push_back(Field::U64(tag::DeliveryId, entry->id));
		response.fields.push_back(Field::U64(tag::PoolGeneration, key.pool.value));
		response.fields.push_back(Field::U64(tag::SharedImageId, key.image.value));
		response.fields.push_back(EncodeImageDescription(entry->value->Description(), true));
		response.fields.push_back(Field::Text(tag::ProducerPhase, CompletionName(entry->value->ProducerCompletion())));
		response.fields.push_back(Field::Text(tag::ConsumerPhase, CompletionName(entry->value->ConsumerCompletion())));
		return response;
	}
	const auto entry = m_exports.Find(U64(request, tag::ExportId));
	if (!entry)
		throw RequestFailure{ErrorCode::NotFound, "Export is absent or released"};
	Source(response, entry->source);
	response.fields.push_back(Field::U64(tag::ImageReaderId, entry->reader));
	response.fields.push_back(Field::U64(tag::ExportId, entry->id));
	ExportSelection(response, *entry->value);
	if (entry->value->MediaType() == "image/raw")
		response.fields.push_back(EncodeImageDescription(entry->value->Description()));
	else
		response.fields.push_back(Field::Text(tag::MediaType, entry->value->MediaType()));
	const auto phase = entry->value->PollCompletion();
	const auto total = entry->value->ByteSize();
	response.fields.push_back(Field::U64(tag::TotalBytes, total));
	response.fields.push_back(Field::Text(tag::Phase, CompletionName(phase)));
	if (phase == CompletionStatus::Failed)
		response.fields.push_back(Field::Text(tag::Detail, entry->value->Error()));
	if (operation == OpCode::ReadExport)
	{
		const auto offset = U64(request, tag::Offset), length = U64(request, tag::Length);
		if (!length || length > MaxPayload - 4096 || offset > total)
			throw RequestFailure{ErrorCode::Invalid, "Invalid bounded export byte range"};
		if (phase == CompletionStatus::Pending)
			throw RequestFailure{ErrorCode::Busy, "Export copy is pending"};
		if (phase == CompletionStatus::Failed)
			throw RequestFailure{ErrorCode::Conflict, "Export copy failed"};
		const auto bytes = entry->value->Read();
		if (bytes.size() != total)
			throw RequestFailure{ErrorCode::Internal, "Export byte count differs from its description"};
		const auto count = static_cast<std::size_t>(std::min(length, total - offset));
		response.fields.push_back(Field::U64(tag::Offset, offset));
		response.fields.push_back(Field::U64(tag::Length, count));
		response.fields.push_back(
			Field::Bytes(tag::Bytes, bytes.subspan(static_cast<std::size_t>(offset), count), true));
		response.fields.push_back(Field::Bool(tag::End, offset + count == total));
	}
	return response;
}
} // namespace simxr::control
