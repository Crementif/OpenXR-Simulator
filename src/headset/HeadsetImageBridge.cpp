#include "headset/HeadsetImageBridge.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "control/LocalControlClient.h"
#include "platform/Clock.h"

namespace simxr::headset
{
namespace wire = control;
std::optional<std::chrono::nanoseconds> HeadsetImageAge(const HeadsetImagePacket& packet,
														std::chrono::steady_clock::time_point now)
{
	using namespace std::chrono;
	if (!packet.sourceAge || packet.sourceAge->count() < 0 || now < packet.requestedAt)
		return {};
	const auto elapsed = duration_cast<nanoseconds>(HostTimeDifference(now, packet.requestedAt));
	if (*packet.sourceAge > nanoseconds::max() - elapsed)
		return {};
	return *packet.sourceAge + elapsed;
}

bool FreshHeadsetImage(const HeadsetImagePacket& packet, std::chrono::nanoseconds period,
					   std::chrono::steady_clock::time_point now)
{
	using namespace std::chrono;
	const auto age = HeadsetImageAge(packet, now);
	if (!age || period <= nanoseconds::zero())
		return false;
	// Provisional four-period bound; clamp before multiplying the display period.
	const auto limit = std::clamp(period, nanoseconds(milliseconds(1)), nanoseconds(milliseconds(50))) * 4;
	return *age < limit;
}
static std::uint64_t ImageId(const wire::Message& message, std::uint16_t tag)
{
	const auto* field = message.Find(tag);
	if (!field || !field->AsU64() || !*field->AsU64())
		throw std::runtime_error("Live headset image omitted an identity");
	return *field->AsU64();
}
HeadsetImageBridge::HeadsetImageBridge(HeadsetImageSource source) : m_source(std::move(source))
{
	if (m_source.endpoint.empty() || !m_source.session || !m_source.generation || m_source.capture)
		throw std::invalid_argument("Live image bridge requires a session target without a capture");
	m_worker = std::jthread(
		[this](std::stop_token stop)
		{
			Run(stop);
		});
}
HeadsetImageBridge::~HeadsetImageBridge()
{
	Stop();
}
bool HeadsetImageBridge::Request(wire::ImageDeviceIdentity identity, std::shared_ptr<wire::TransferHandle> completion,
								 std::uint64_t afterImageTick, ImageCompletionKind completionKind)
{
	if (!completion)
		throw std::invalid_argument("Live image request requires a consumer completion handle");
	std::lock_guard lock(m_mutex);
	if (m_phase != Phase::Idle)
		return false;
	m_offer = Offer{identity, std::move(completion), afterImageTick, completionKind};
	m_phase = Phase::Requested;
	m_changed.notify_all();
	return true;
}
std::optional<HeadsetImagePacket> HeadsetImageBridge::Take()
{
	std::lock_guard lock(m_mutex);
	if (m_phase != Phase::Delivered || !m_packet)
		return {};
	auto packet = std::move(m_packet);
	m_packet.reset();
	if (packet->kind != HeadsetImageKind::Image)
		m_phase = Phase::Idle;
	return packet;
}
bool HeadsetImageBridge::Acknowledge(std::uint64_t delivery)
{
	std::lock_guard lock(m_mutex);
	if (m_phase != Phase::Delivered || m_packet || !delivery || delivery != m_delivery)
		return false;
	m_phase = Phase::ReleaseRequested;
	m_changed.notify_all();
	return true;
}
std::string HeadsetImageBridge::Error() const
{
	std::lock_guard lock(m_mutex);
	return m_phase == Phase::Failed && m_error.empty() ? "Live image connection failed" : m_error;
}
void HeadsetImageBridge::Stop()
{
	m_worker.request_stop();
	std::shared_ptr<wire::LocalControlClient> client;
	{
		std::lock_guard lock(m_mutex);
		client = m_client;
	}
	if (client)
		client->Close();
	m_changed.notify_all();
	if (m_worker.joinable())
		m_worker.join();
	std::lock_guard lock(m_mutex);
	if (m_phase != Phase::Failed)
		m_phase = Phase::Stopped;
}
void HeadsetImageBridge::Run(std::stop_token stop) noexcept
{
	try
	{
		auto client = std::make_shared<wire::LocalControlClient>(m_source.endpoint, std::chrono::milliseconds(250));
		{
			std::lock_guard lock(m_mutex);
			m_client = client;
		}
		const auto call = [&](wire::OpCode opcode, std::vector<wire::Field> fields = {},
							  std::vector<std::shared_ptr<wire::TransferHandle>> handles = {}, bool unavailable = false)
		{
			if (stop.stop_requested())
				throw std::runtime_error("Image connection stopped");
			wire::ControlPacket request;
			request.message.opcode = static_cast<std::uint16_t>(opcode);
			request.message.fields = std::move(fields);
			request.handles = std::move(handles);
			auto response = client->Request(std::move(request));
			const auto* error = response.message.Find(wire::tag::Error);
			if (!error || !error->AsU32())
				throw std::runtime_error("Image response omitted result");
			const auto code = static_cast<wire::ErrorCode>(*error->AsU32());
			if (unavailable && code == wire::ErrorCode::Busy)
				return response;
			if (code != wire::ErrorCode::Ok)
			{
				const auto* detail = response.message.Find(wire::tag::Detail);
				throw std::runtime_error(detail && detail->AsText() ? std::string(*detail->AsText())
																	: "Image request failed");
			}
			return response;
		};
		call(wire::OpCode::Hello, {wire::Field::Text(wire::tag::ClientName, "OpenXR headset live images"),
								   wire::Field::U64(wire::tag::RequiredCapabilities,
													static_cast<std::uint64_t>(wire::Capability::GpuPreview) |
														static_cast<std::uint64_t>(wire::Capability::LivePreview))});
		std::uint64_t reader = 0, delivery = 0;
		while (!stop.stop_requested())
		{
			std::optional<Offer> offer;
			bool release = false;
			{
				std::unique_lock lock(m_mutex);
				m_changed.wait(lock,
							   [&]
							   {
								   return stop.stop_requested() || m_phase == Phase::Requested ||
										  m_phase == Phase::ReleaseRequested;
							   });
				if (stop.stop_requested())
					break;
				release = m_phase == Phase::ReleaseRequested;
				if (!release)
				{
					offer = std::move(m_offer);
					m_offer.reset();
				}
			}
			if (release)
			{
				call(wire::OpCode::ReleaseDelivery, {wire::Field::U64(wire::tag::DeliveryId, delivery)});
				call(wire::OpCode::ReleaseImage, {wire::Field::U64(wire::tag::ImageReaderId, reader)});
				reader = delivery = 0;
				std::lock_guard lock(m_mutex);
				m_delivery = 0;
				m_phase = Phase::Idle;
				continue;
			}
			const auto requestedAt = std::chrono::steady_clock::now();
			auto retained = call(wire::OpCode::RetainPreview,
								 {wire::Field::U64(wire::tag::Session, m_source.session),
								  wire::Field::U64(wire::tag::Generation, m_source.generation)},
								 {}, true);
			HeadsetImagePacket packet;
			packet.requestedAt = requestedAt;
			const auto* age = retained.message.Find(wire::tag::SourceAge);
			if (age && age->AsI64() && *age->AsI64() >= 0)
				packet.sourceAge = std::chrono::nanoseconds(*age->AsI64());
			if (retained.message.Find(wire::tag::Error)->AsU32() == static_cast<std::uint32_t>(wire::ErrorCode::Ok))
			{
				if (ImageId(retained.message, wire::tag::Session) != m_source.session ||
					ImageId(retained.message, wire::tag::Generation) != m_source.generation)
					throw std::runtime_error("Live image target identity changed");
				const auto* available = retained.message.Find(wire::tag::ImageAvailable);
				if (!available || !available->AsBool())
					throw std::runtime_error("Live image response omitted availability");
				packet.kind = HeadsetImageKind::Clear;
				if (*available->AsBool())
				{
					packet.session = m_source.session;
					const auto* frame = retained.message.Find(wire::tag::Frame);
					if (!frame)
						throw std::runtime_error("Live image omitted its source frame");
					auto decoded = wire::DecodeFields(frame->value);
					if (decoded.error != wire::DecodeError::None)
						throw std::runtime_error("Live image source frame is malformed");
					wire::Message key;
					key.fields = std::move(decoded.fields);
					packet.frame = {{ImageId(key, wire::tag::Generation)}, {ImageId(key, wire::tag::FrameId)}};
					if (packet.frame.generation.value != m_source.generation)
						throw std::runtime_error("Live image source frame generation changed");
					reader = ImageId(retained.message, wire::tag::ImageReaderId);
					packet.imageTick = ImageId(retained.message, wire::tag::ImageTick);
					packet.kind = HeadsetImageKind::Unchanged;
					if (packet.imageTick != offer->afterImageTick)
					{
						auto shared = call(wire::OpCode::ShareImage,
										   {wire::Field::U64(wire::tag::ImageReaderId, reader),
											wire::Field::Bytes(wire::tag::DeviceUuid, offer->identity.device),
											wire::Field::Bytes(wire::tag::DriverUuid, offer->identity.driver),
											wire::Field::U32(wire::tag::HandleCount, 1),
											wire::Field::U32(wire::tag::CompletionKind,
															 static_cast<std::uint32_t>(offer->completionKind))},
										   {offer->completion});
						delivery = ImageId(shared.message, wire::tag::DeliveryId);
						ImageId(shared.message, wire::tag::PoolGeneration);
						ImageId(shared.message, wire::tag::SharedImageId);
						const auto* handleCount = shared.message.Find(wire::tag::HandleCount);
						if (!handleCount || handleCount->AsU32() != shared.handles.size())
							throw std::runtime_error("Live image handle count mismatch");
						const auto* description = shared.message.Find(wire::tag::ImageDescriptor);
						if (!description)
							throw std::runtime_error("Live image omitted its description");
						packet.description = wire::DecodeImageDescription(*description);
						if (packet.description.completionKind != offer->completionKind)
							throw std::runtime_error("Live image completion kind changed");
						packet.delivery = delivery;
						packet.handles = std::move(shared.handles);
						packet.kind = HeadsetImageKind::Image;
					}
					else
					{
						call(wire::OpCode::ReleaseImage, {wire::Field::U64(wire::tag::ImageReaderId, reader)});
						reader = 0;
					}
				}
			}
			packet.source = std::move(retained.message);
			std::lock_guard lock(m_mutex);
			m_delivery = delivery;
			m_packet = std::move(packet);
			m_phase = Phase::Delivered;
		}
		client->Close();
	}
	catch (const std::exception& error)
	{
		std::shared_ptr<wire::LocalControlClient> client;
		{
			std::lock_guard lock(m_mutex);
			client = m_client;
			if (!stop.stop_requested())
			{
				m_phase = Phase::Failed;
				try
				{
					m_error = error.what();
				}
				catch (...)
				{
				}
			}
		}
		if (client)
			client->Close();
	}
}
} // namespace simxr::headset
