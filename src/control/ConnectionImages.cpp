#include "graphics/SnapshotBudget.h"
#include "control/ConnectionImages.h"
#include "control/DebugProtocol.h"
#include "control/ControlSchema.h"

#include <algorithm>
#include <limits>
#include <utility>

namespace simxr::control
{
namespace
{
template <class Resource> struct ImageEntry
{
	std::uint64_t id = 0, reader = 0;
	ImageSource source;
	std::shared_ptr<Resource> value;
};
template <class Resource> class ImageTable
{
  public:
	ImageTable()
	{
		m_entries.reserve(SnapshotBudget::Capacity());
	}
	void CheckCapacity() const
	{
		if (m_entries.size() >= SnapshotBudget::Capacity() || m_next == std::numeric_limits<std::uint64_t>::max())
			throw ConnectionImages::RequestFailure{ErrorCode::Capacity, "Connection image resource capacity exhausted"};
	}
	std::shared_ptr<const ImageEntry<Resource>> Find(std::uint64_t id) const
	{
		for (const auto& entry : m_entries)
			if (entry->id == id)
				return entry;
		return {};
	}
	std::shared_ptr<const ImageEntry<Resource>> Find(const ImageSource& source) const
	{
		for (const auto& entry : m_entries)
			if (entry->source == source)
				return entry;
		return {};
	}
	std::shared_ptr<const ImageEntry<Resource>> Add(std::shared_ptr<Resource> value, ImageSource source,
													std::uint64_t reader = 0)
	{
		CheckCapacity();
		auto entry =
			std::make_shared<ImageEntry<Resource>>(ImageEntry<Resource>{m_next, reader, source, std::move(value)});
		m_entries.push_back(entry);
		++m_next;
		return entry;
	}
	std::shared_ptr<const ImageEntry<Resource>> Release(std::uint64_t id) noexcept
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

  private:
	std::vector<std::shared_ptr<ImageEntry<Resource>>> m_entries;
	std::uint64_t m_next = 1;
};
Message Response(const Message& request, ErrorCode error = ErrorCode::Ok, const char* detail = nullptr)
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
std::uint64_t U64(const Message& message, std::uint16_t name)
{
	const auto* field = message.Find(name);
	const auto value = field ? field->AsU64() : std::nullopt;
	if (!value)
		throw ConnectionImages::RequestFailure{ErrorCode::Invalid, "Missing or invalid uint64 field"};
	return *value;
}
std::uint32_t U32(const Message& message, std::uint16_t name, std::optional<std::uint32_t> fallback = {})
{
	const auto* field = message.Find(name);
	if (!field && fallback)
		return *fallback;
	const auto value = field ? field->AsU32() : std::nullopt;
	if (!value)
		throw ConnectionImages::RequestFailure{ErrorCode::Invalid, "Missing or invalid uint32 field"};
	return *value;
}
Field Nested(std::uint16_t name, std::vector<Field> fields)
{
	return Field::Bytes(name, EncodeFields(fields));
}
Field Key(std::uint16_t name, FrameKey key)
{
	return Nested(name, {Field::U64(tag::Generation, key.generation.value), Field::U64(tag::FrameId, key.frame.value)});
}
const char* CompletionName(CompletionStatus status)
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
std::uint64_t ImageMask(ImageCapabilities capabilities, bool handles)
{
	return (capabilities.imageExport ? static_cast<std::uint64_t>(Capability::ImageExport) : 0) |
		   (capabilities.gpuPreview && handles ? static_cast<std::uint64_t>(Capability::GpuPreview) : 0);
}
void Source(Message& response, const ImageSource& source, bool session = true)
{
	if (session)
	{
		response.fields.push_back(Field::U64(tag::Session, source.session.value));
		response.fields.push_back(Field::U64(tag::Generation, source.frame.generation.value));
	}
	response.fields.push_back(Key(tag::Frame, source.frame));
	response.fields.push_back(Field::U64(tag::CaptureId, source.capture.value));
	response.fields.push_back(EncodeCaptureTarget(source.target));
}
ImageDeviceIdentity DeviceIdentity(const Message& message)
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
} // namespace

struct ConnectionImages::State
{
	ImageTable<ImageReader> images;
	ImageTable<ImageDelivery> deliveries;
	ImageTable<ImageExport> exports;
};

ConnectionImages::ConnectionImages() : m_state(std::make_unique<State>()) {}
ConnectionImages::~ConnectionImages() = default;

ConnectionImages::Publication::Publication(ConnectionImages& owner, RetiredResource& retired)
	: m_owner(owner), m_retired(retired)
{
}
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
		return m_state->images.Release(id);
	case ResourceKind::Delivery:
		return m_state->deliveries.Release(id);
	case ResourceKind::Export:
		return m_state->exports.Release(id);
	}
	return {};
}

void ConnectionImages::Retain(Message& response, ImageSource source, std::shared_ptr<const FrameOutput> output,
							  const ImageInspectionProvider& provider, bool nativeHandles, Publication& publication,
							  bool includeSession)
{
	auto entry = m_state->images.Find(source);
	if (!entry)
	{
		m_state->images.CheckCapacity();
		auto reader = provider.Retain(std::move(output));
		if (!reader)
		{
			const auto* detail = source.target.kind == CaptureKind::VirtualDisplay
									 ? "Selected image backend cannot retain preview"
									 : "Capture backend cannot retain this image";
			throw RequestFailure{ErrorCode::UnsupportedCommand, detail};
		}
		entry = m_state->images.Add(std::move(reader), source);
		publication.Track(ResourceKind::Reader, entry->id);
	}
	Source(response, source, includeSession);
	response.fields.push_back(Field::U64(tag::ImageReaderId, entry->id));
	response.fields.push_back(EncodeImageDescription(entry->value->Description()));
	response.fields.push_back(Field::U64(tag::Capabilities, ImageMask(entry->value->Capabilities(), nativeHandles)));
	response.fields.push_back(Field::Text(tag::Phase, CompletionName(entry->value->PollCompletion())));
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
		const auto image = m_state->images.Find(U64(request, tag::ImageReaderId));
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
			m_state->deliveries.CheckCapacity();
			auto delivery = image->value->Share(DeviceIdentity(request), incoming.handles.front());
			if (!delivery)
				throw RequestFailure{ErrorCode::Internal, "Image provider returned no delivery"};
			const auto entry = m_state->deliveries.Add(std::move(delivery), image->source, image->id);
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
		m_state->exports.CheckCapacity();
		const auto layer = request.Find(tag::Layer) ? std::optional{U32(request, tag::Layer)} : std::nullopt;
		auto readback = image->value->Export(layer);
		if (!readback)
			throw RequestFailure{ErrorCode::Internal, "Image provider returned no export"};
		const auto entry = m_state->exports.Add(std::move(readback), image->source, image->id);
		publication.Track(ResourceKind::Export, entry->id);
		response.fields.push_back(Field::U64(tag::ExportId, entry->id));
		response.fields.push_back(EncodeImageDescription(entry->value->Description()));
		response.fields.push_back(Field::U64(tag::TotalBytes, entry->value->ByteSize()));
		response.fields.push_back(Field::Text(tag::Phase, CompletionName(entry->value->PollCompletion())));
		return response;
	}
	if (operation == OpCode::DeliveryInfo)
	{
		const auto entry = m_state->deliveries.Find(U64(request, tag::DeliveryId));
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
	const auto entry = m_state->exports.Find(U64(request, tag::ExportId));
	if (!entry)
		throw RequestFailure{ErrorCode::NotFound, "Export is absent or released"};
	Source(response, entry->source);
	response.fields.push_back(Field::U64(tag::ImageReaderId, entry->reader));
	response.fields.push_back(Field::U64(tag::ExportId, entry->id));
	response.fields.push_back(EncodeImageDescription(entry->value->Description()));
	const auto total = entry->value->ByteSize();
	const auto phase = entry->value->PollCompletion();
	response.fields.push_back(Field::U64(tag::TotalBytes, total));
	response.fields.push_back(Field::Text(tag::Phase, CompletionName(phase)));
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
