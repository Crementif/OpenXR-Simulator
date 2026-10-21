#include "control/ImageInspection.h"
#include "control/ControlSchema.h"

#include <algorithm>
#include <array>
#include <stdexcept>
#include <utility>
#include <vector>

namespace simxr::control
{
Field EncodeImageDescription(const ImageDescription& value, bool external)
{
	const std::vector<Field> fields{
		Field::Bytes(tag::DeviceUuid, value.identity.device, true),
		Field::Bytes(tag::DriverUuid, value.identity.driver, true),
		Field::U32(tag::Format, value.format, true),
		Field::U32(tag::Width, value.width, true),
		Field::U32(tag::Height, value.height, true),
		Field::U32(tag::Depth, value.depth, true),
		Field::U32(tag::ArrayLayers, value.arrayLayers, true),
		Field::U32(tag::MipLevels, value.mipLevels, true),
		Field::U32(tag::Samples, value.samples, true),
		Field::U32(tag::Usage, value.usage, true),
		Field::U32(tag::Tiling, value.tiling, true),
		Field::U32(tag::ImageType, value.imageType, true),
		Field::U32(tag::ImageFlags, value.flags, true),
		Field::U64(tag::AllocationSize, value.allocationSize, true),
		Field::U32(tag::MemoryTypeIndex, value.memoryTypeIndex, true),
		Field::Bool(tag::Dedicated, value.dedicated, true),
		Field::U32(tag::ProducerQueueFamily, value.producerQueueFamily, true),
		Field::U32(tag::Layout, value.layout, true),
		Field::U32(tag::HandleKind, external ? static_cast<std::uint32_t>(TransferHandleKind::OpaqueFd) : 0, true)};
	return Field::Bytes(tag::ImageDescriptor, EncodeFields(fields));
}

ImageDescription DecodeImageDescription(const Field& field)
{
	auto fields = DecodeFields(field.value);
	if (fields.error != DecodeError::None)
		throw std::invalid_argument("Invalid image description fields");
	Message message;
	message.fields = std::move(fields.fields);
	constexpr std::array known{
		tag::DeviceUuid, tag::DriverUuid,		   tag::Format,		tag::Width,			 tag::Height,
		tag::Depth,		 tag::ArrayLayers,		   tag::MipLevels,	tag::Samples,		 tag::Usage,
		tag::Tiling,	 tag::ImageType,		   tag::ImageFlags, tag::AllocationSize, tag::MemoryTypeIndex,
		tag::Dedicated,	 tag::ProducerQueueFamily, tag::Layout,		tag::HandleKind};
	if (message.ValidateFields(known))
		throw std::invalid_argument("Unknown required image description field");
	const auto device = message.Find(tag::DeviceUuid);
	const auto driver = message.Find(tag::DriverUuid);
	const auto allocation = message.Find(tag::AllocationSize);
	const auto dedicated = message.Find(tag::Dedicated);
	if (!device || !driver || device->value.size() != 16 || driver->value.size() != 16 || !allocation ||
		!allocation->AsU64() || !dedicated || !dedicated->AsBool())
		throw std::invalid_argument("Invalid image identity or allocation metadata");
	ImageDescription result;
	std::copy(device->value.begin(), device->value.end(), result.identity.device.begin());
	std::copy(driver->value.begin(), driver->value.end(), result.identity.driver.begin());
	result.format = RequireRequestU32(message, tag::Format);
	result.width = RequireRequestU32(message, tag::Width);
	result.height = RequireRequestU32(message, tag::Height);
	result.depth = RequireRequestU32(message, tag::Depth);
	result.arrayLayers = RequireRequestU32(message, tag::ArrayLayers);
	result.mipLevels = RequireRequestU32(message, tag::MipLevels);
	result.samples = RequireRequestU32(message, tag::Samples);
	result.usage = RequireRequestU32(message, tag::Usage);
	result.tiling = RequireRequestU32(message, tag::Tiling);
	result.imageType = RequireRequestU32(message, tag::ImageType);
	result.flags = RequireRequestU32(message, tag::ImageFlags);
	result.memoryTypeIndex = RequireRequestU32(message, tag::MemoryTypeIndex);
	result.producerQueueFamily = RequireRequestU32(message, tag::ProducerQueueFamily);
	result.layout = RequireRequestU32(message, tag::Layout);
	result.handleKind = RequireRequestU32(message, tag::HandleKind);
	result.allocationSize = *allocation->AsU64();
	result.dedicated = *dedicated->AsBool();
	if (result.handleKind > static_cast<std::uint32_t>(TransferHandleKind::OpaqueFd) ||
		(result.handleKind && !result.allocationSize))
		throw std::invalid_argument("Invalid image sharing handle kind");
	return result;
}
} // namespace simxr::control
