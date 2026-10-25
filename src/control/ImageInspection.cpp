#include "control/ImageInspection.h"
#include "Pch.h"
#include "math/Math.h"
#include "control/ControlSchema.h"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace simxr::control
{
Field EncodeImageDescription(const ImageDescription& value, bool external)
{
	std::vector<Field> fields{Field::Bytes(tag::DeviceUuid, value.identity.device, true),
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
							  Field::U32(tag::HandleKind, external ? value.handleKind : 0, true)};
	if (value.completionKind != ImageCompletionKind::Fence)
		fields.push_back(Field::U32(tag::CompletionKind, static_cast<std::uint32_t>(value.completionKind), true));
	if (!value.views.empty())
	{
		std::vector<Field> views;
		for (const auto& view : value.views)
		{
			const std::array values{view.pose.orientation.x, view.pose.orientation.y, view.pose.orientation.z,
									view.pose.orientation.w, view.pose.position.x,	  view.pose.position.y,
									view.pose.position.z,	 view.fov.angleLeft,	  view.fov.angleRight,
									view.fov.angleUp,		 view.fov.angleDown};
			std::vector<Field> components;
			for (float component : values)
				components.push_back(Field::F32(static_cast<std::uint16_t>(components.size() + 1), component, true));
			views.push_back(Field::Bytes(static_cast<std::uint16_t>(views.size() + 1), EncodeFields(components), true));
		}
		fields.push_back(Field::Bytes(tag::ImageViews, EncodeFields(views)));
	}
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
		tag::DeviceUuid,	tag::DriverUuid,		  tag::Format,	   tag::Width,			tag::Height,
		tag::Depth,			tag::ArrayLayers,		  tag::MipLevels,  tag::Samples,		tag::Usage,
		tag::Tiling,		tag::ImageType,			  tag::ImageFlags, tag::AllocationSize, tag::MemoryTypeIndex,
		tag::Dedicated,		tag::ProducerQueueFamily, tag::Layout,	   tag::HandleKind,		tag::ImageViews,
		tag::CompletionKind};
	if (message.ValidateFields(known))
		throw std::invalid_argument("Unknown required image description field");
	const auto device = message.Find(tag::DeviceUuid);
	const auto driver = message.Find(tag::DriverUuid);
	const auto allocation = message.Find(tag::AllocationSize);
	const auto dedicated = message.Find(tag::Dedicated);
	if (!device || !driver || device->value.size() != 16 || driver->value.size() != 16 || !allocation ||
		!allocation->AsU64() || !dedicated || !dedicated->AsBool())
		throw std::invalid_argument("Invalid image identity or allocation metadata");
	const auto u32 = [&](std::uint16_t name)
	{
		const auto* scalar = message.Find(name);
		const auto value = scalar ? scalar->AsU32() : std::nullopt;
		if (!value)
			throw std::invalid_argument("Missing or invalid image description uint32 field");
		return *value;
	};
	ImageDescription result;
	std::copy(device->value.begin(), device->value.end(), result.identity.device.begin());
	std::copy(driver->value.begin(), driver->value.end(), result.identity.driver.begin());
	result.format = u32(tag::Format);
	result.width = u32(tag::Width);
	result.height = u32(tag::Height);
	result.depth = u32(tag::Depth);
	result.arrayLayers = u32(tag::ArrayLayers);
	result.mipLevels = u32(tag::MipLevels);
	result.samples = u32(tag::Samples);
	result.usage = u32(tag::Usage);
	result.tiling = u32(tag::Tiling);
	result.imageType = u32(tag::ImageType);
	result.flags = u32(tag::ImageFlags);
	result.memoryTypeIndex = u32(tag::MemoryTypeIndex);
	result.producerQueueFamily = u32(tag::ProducerQueueFamily);
	result.layout = u32(tag::Layout);
	result.handleKind = u32(tag::HandleKind);
	if (const auto completion = message.Find(tag::CompletionKind))
	{
		const auto value = completion->AsU32();
		if (!value || *value > static_cast<std::uint32_t>(ImageCompletionKind::Timeline))
			throw std::invalid_argument("Invalid image completion kind");
		result.completionKind = static_cast<ImageCompletionKind>(*value);
	}
	result.allocationSize = *allocation->AsU64();
	result.dedicated = *dedicated->AsBool();
	if (result.handleKind > static_cast<std::uint32_t>(TransferHandleKind::OpaqueWin32) ||
		(result.handleKind && !result.allocationSize))
		throw std::invalid_argument("Invalid image sharing handle kind");
	if (const auto metadata = message.Find(tag::ImageViews))
	{
		const auto views = DecodeFields(metadata->value);
		if (views.error != DecodeError::None || views.fields.size() > 4 || views.fields.size() != result.arrayLayers)
			throw std::invalid_argument("Invalid image view count");
		for (std::size_t index = 0; index < views.fields.size(); ++index)
		{
			const auto& field = views.fields[index];
			const auto components = DecodeFields(field.value);
			if (field.tag != index + 1 || components.error != DecodeError::None || components.fields.size() != 11)
				throw std::invalid_argument("Invalid image view components");
			std::array<float, 11> values{};
			for (std::size_t component = 0; component < values.size(); ++component)
			{
				const auto& number = components.fields[component];
				const auto value = number.AsF32();
				if (number.tag != component + 1 || !value || !std::isfinite(*value))
					throw std::invalid_argument("Invalid image view value");
				values[component] = *value;
			}
			FrameView view{{{values[0], values[1], values[2], values[3]}, {values[4], values[5], values[6]}},
						   {values[7], values[8], values[9], values[10]}};
			const auto pose = NormalizedPose(view.pose);
			if (!pose || values[7] >= values[8] || values[10] >= values[9] ||
				std::any_of(values.begin() + 7, values.end(),
							[](float angle)
							{
								return std::abs(angle) >= std::numbers::pi_v<float> / 2;
							}))
				throw std::invalid_argument("Invalid image view pose or FOV");
			view.pose = *pose;
			result.views.push_back(view);
		}
	}
	return result;
}
} // namespace simxr::control
