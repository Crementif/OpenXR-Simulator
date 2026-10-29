#include "headset/HeadsetImageProbe.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "control/LocalControlClient.h"
#include "control/backends/vulkan/VulkanImageConsumer.h"
#include "headset/HeadsetGraphics.h"
#include "headset/HeadsetImages.h"

#include <iostream>

namespace simxr::headset
{
static std::uint64_t ImageResultId(const control::ControlPacket& packet, std::uint16_t tag)
{
	const auto field = packet.message.Find(tag);
	if (!field || !field->AsU64() || !*field->AsU64())
		throw std::runtime_error("Headset image response omitted an identity");
	return *field->AsU64();
}

void VerifyHeadsetImageCopy(HeadsetGraphics& graphics, const HeadsetImageSource& source)
{
	namespace wire = control;
	namespace tag = wire::tag;
	if (source.endpoint.empty() || !source.session || !source.generation)
		throw std::invalid_argument("Headset image copy requires endpoint, session and generation");
	if (!source.capture)
	{
		HeadsetImages images(graphics, source);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (!images.Current())
		{
			images.Tick();
			if (std::chrono::steady_clock::now() >= deadline)
				throw std::runtime_error("No completed live headset image arrived before the copy deadline");
			if (!images.Current())
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		const auto description = images.Current()->metadata.description;
		if (!images.Close())
			throw std::runtime_error("Live headset image copy could not retire its native resources");
		std::cout << "shared_image_source=live_display\nshared_image_copy=complete\nshared_image_size="
				  << description.width << 'x' << description.height << " layers=" << description.arrayLayers << '\n';
		return;
	}
	wire::LocalControlClient client(source.endpoint);
	const auto call = [&](wire::OpCode opcode, std::vector<wire::Field> fields = {},
						  std::vector<std::shared_ptr<wire::TransferHandle>> handles = {}) {
		wire::ControlPacket request;
		request.message.opcode = static_cast<std::uint16_t>(opcode);
		request.message.fields = std::move(fields);
		request.handles = std::move(handles);
		auto response = client.Request(std::move(request));
		const auto error = response.message.Find(tag::Error);
		if (!error || error->AsU32() != static_cast<std::uint32_t>(wire::ErrorCode::Ok))
		{
			const auto detail = response.message.Find(tag::Detail);
			throw std::runtime_error(detail && detail->AsText() ? std::string(*detail->AsText())
																: "Headset image control request failed");
		}
		return response;
	};
	call(wire::OpCode::Hello,
		 {wire::Field::Text(tag::ClientName, "OpenXR headset image probe"),
		  wire::Field::U64(tag::RequiredCapabilities, static_cast<std::uint64_t>(wire::Capability::GpuPreview))});
	const auto retained = call(wire::OpCode::RetainImage, {wire::Field::U64(tag::Session, source.session),
														   wire::Field::U64(tag::Generation, source.generation),
														   wire::Field::U64(tag::CaptureId, source.capture)});
	const auto reader = ImageResultId(retained, tag::ImageReaderId);
	if (ImageResultId(retained, tag::Session) != source.session ||
		ImageResultId(retained, tag::Generation) != source.generation)
		throw std::runtime_error("Headset image reader changed simulator session identity");
	// This synchronous diagnostic borrows the native device. Every exit either
	// proves consumer retirement or preserves the host's native owners.
	auto device = std::make_shared<VulkanDevice>(
		VulkanBinding{graphics.Instance(), graphics.PhysicalDevice(), graphics.Device(), graphics.QueueFamily(), 0});
	wire::VulkanImageConsumer consumer(device, graphics.Extensions(), graphics.ApiVersion(),
									   ImageCompletionKind::Timeline, true);
	try
	{
		const auto identity = consumer.Identity();
		const auto shared =
			call(wire::OpCode::ShareImage,
				 {wire::Field::U64(tag::ImageReaderId, reader), wire::Field::Bytes(tag::DeviceUuid, identity.device),
				  wire::Field::Bytes(tag::DriverUuid, identity.driver), wire::Field::U32(tag::HandleCount, 1),
				  wire::Field::U32(tag::CompletionKind, static_cast<std::uint32_t>(ImageCompletionKind::Timeline))},
				 {consumer.ConsumerFence()});
		const auto delivery = ImageResultId(shared, tag::DeliveryId);
		const auto descriptor = shared.message.Find(tag::ImageDescriptor);
		if (!descriptor || !ImageResultId(shared, tag::PoolGeneration) || !ImageResultId(shared, tag::SharedImageId))
			throw std::runtime_error("Headset image delivery omitted its descriptor or resource identity");
		const auto description = wire::DecodeImageDescription(*descriptor);
		consumer.Import(description, shared.handles);
		consumer.Submit();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (true)
		{
			const auto status = consumer.Poll();
			if (status == CompletionStatus::Complete)
				break;
			if (status == CompletionStatus::Failed || std::chrono::steady_clock::now() >= deadline)
				throw std::runtime_error("Headset shared-image GPU copy failed or timed out");
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
		call(wire::OpCode::ReleaseDelivery, {wire::Field::U64(tag::DeliveryId, delivery)});
		call(wire::OpCode::ReleaseImage, {wire::Field::U64(tag::ImageReaderId, reader)});
		std::cout << "shared_image_source=capture\n";
		std::cout << "shared_image_copy=complete\nshared_image_size=" << description.width << 'x' << description.height
				  << " layers=" << description.arrayLayers << '\n';
	}
	catch (...)
	{
		client.Close();
		try
		{
			consumer.CancelUnused();
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
			while (!consumer.NativeIdle() && std::chrono::steady_clock::now() < deadline)
			{
				consumer.Poll();
				if (!consumer.NativeIdle())
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
		}
		catch (...)
		{}
		if (!consumer.NativeIdle() || device->lost)
			graphics.PreserveNativeResources();
		throw;
	}
}
} // namespace simxr::headset
