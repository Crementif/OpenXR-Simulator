#include "control/backends/vulkan/VulkanImageInspection.h"
#include "Pch.h"
#include "graphics/ColorPixels.h"
#include "control/backends/vulkan/VulkanControlHandle.h"
#include "graphics/backends/vulkan/VulkanInspection.h"

#include <mutex>
#include <stdexcept>

namespace simxr::control
{
template <class Function> static auto Translate(Function&& function)
{
	try
	{
		return function();
	}
	catch (const VulkanError& error)
	{
		ErrorCode code = ErrorCode::Internal;
		switch (error.Result())
		{
		case VK_NOT_READY:
			code = ErrorCode::Busy;
			break;
		case VK_ERROR_OUT_OF_HOST_MEMORY:
		case VK_ERROR_OUT_OF_DEVICE_MEMORY:
		case VK_ERROR_OUT_OF_POOL_MEMORY:
			code = ErrorCode::Capacity;
			break;
		case VK_ERROR_FEATURE_NOT_PRESENT:
		case VK_ERROR_EXTENSION_NOT_PRESENT:
		case VK_ERROR_FORMAT_NOT_SUPPORTED:
		case VK_ERROR_INCOMPATIBLE_DRIVER:
			code = ErrorCode::UnsupportedCommand;
			break;
		case VK_ERROR_INVALID_EXTERNAL_HANDLE:
			code = ErrorCode::Invalid;
			break;
		case VK_ERROR_DEVICE_LOST:
			code = ErrorCode::Closed;
			break;
		default:
			break;
		}
		throw ImageInspectionError(code, error.what());
	}
}
static ImageDeviceIdentity Identity(const VulkanExternalIdentity& value)
{
	ImageDeviceIdentity result;
	std::transform(value.deviceUuid.begin(), value.deviceUuid.end(), result.device.begin(),
				   [](auto byte)
				   {
					   return static_cast<std::byte>(byte);
				   });
	std::transform(value.driverUuid.begin(), value.driverUuid.end(), result.driver.begin(),
				   [](auto byte)
				   {
					   return static_cast<std::byte>(byte);
				   });
	return result;
}
static VulkanExternalIdentity Identity(const ImageDeviceIdentity& value)
{
	VulkanExternalIdentity result;
	std::transform(value.device.begin(), value.device.end(), result.deviceUuid.begin(),
				   [](auto byte)
				   {
					   return std::to_integer<std::uint8_t>(byte);
				   });
	std::transform(value.driver.begin(), value.driver.end(), result.driverUuid.begin(),
				   [](auto byte)
				   {
					   return std::to_integer<std::uint8_t>(byte);
				   });
	return result;
}
static ImageDescription Describe(const VulkanExternalImageDescriptor& value)
{
	ImageDescription result;
	result.identity = Identity(value.identity);
	const auto& image = value.image;
	result.format = image.format;
	result.width = image.extent.width;
	result.height = image.extent.height;
	result.depth = image.extent.depth;
	result.arrayLayers = image.arrayLayers;
	result.mipLevels = image.mipLevels;
	result.samples = image.samples;
	result.usage = image.usage;
	result.tiling = image.tiling;
	result.imageType = image.imageType;
	result.flags = image.flags;
	result.handleKind = static_cast<std::uint32_t>(VulkanTransferKind);
	result.allocationSize = value.allocationSize;
	result.memoryTypeIndex = value.memoryTypeIndex;
	result.dedicated = value.dedicated;
	result.producerQueueFamily = value.producerQueueFamily;
	result.layout = value.producerLayout;
	return result;
}
static ImageDescription Describe(const VulkanInspectionFrame& frame)
{
	const auto& image = frame.Image();
	VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};
	vkGetPhysicalDeviceProperties2(image.device->binding.physicalDevice, &properties);
	VulkanExternalIdentity native;
	std::copy_n(identity.deviceUUID, VK_UUID_SIZE, native.deviceUuid.begin());
	std::copy_n(identity.driverUUID, VK_UUID_SIZE, native.driverUuid.begin());
	ImageDescription result;
	result.identity = Identity(native);
	result.format = image.format;
	result.width = image.extent.width;
	result.height = image.extent.height;
	result.arrayLayers = image.layers;
	result.usage = image.usage;
	result.producerQueueFamily = image.device->binding.queueFamily;
	result.layout = VK_IMAGE_LAYOUT_GENERAL;
	result.views.assign(frame.Views().begin(), frame.Views().end());
	result.completionKind = frame.CompletionKind();
	return result;
}
struct VulkanExportCharge;
struct VulkanExportBudget
{
	std::mutex mutex;
	std::uint64_t bytes = 0;
	std::size_t count = 0;
	std::array<std::weak_ptr<VulkanExportCharge>, 8> charges;
};
struct VulkanExportCharge
{
	std::shared_ptr<VulkanExportBudget> state;
	std::function<void()> collect;
	std::uint64_t bytes = 0;
	bool charged = false;
	~VulkanExportCharge()
	{
		if (charged)
		{
			std::lock_guard lock(state->mutex);
			state->bytes -= bytes;
			--state->count;
		}
	}
};
static const std::shared_ptr<VulkanExportBudget>& ExportBudget()
{
	static const auto s_state = std::make_shared<VulkanExportBudget>();
	return s_state;
}
static std::shared_ptr<const void> ReserveExport(std::uint64_t bytes, std::function<void()> collect)
{
	constexpr std::uint64_t limit = 512ULL * 1024 * 1024;
	auto state = ExportBudget();
	std::array<std::shared_ptr<VulkanExportCharge>, 8> observed;
	{
		std::lock_guard lock(state->mutex);
		for (std::size_t index = 0; index < observed.size(); ++index)
			observed[index] = state->charges[index].lock();
	}
	// Released tickets may have completed after their service/session stopped.
	// Poll only owned queues, outside the byte-budget mutex. Weak callbacks
	// never keep those queues alive; actual native work retains its charge.
	for (const auto& charge : observed)
		if (charge)
			try
			{
				charge->collect();
			}
			catch (...)
			{
			}
	observed.fill({});
	auto charge = std::make_shared<VulkanExportCharge>();
	charge->state = state;
	charge->collect = std::move(collect);
	charge->bytes = bytes;
	std::lock_guard lock(state->mutex);
	if (!bytes || bytes > limit || state->count == state->charges.size() || bytes > limit - state->bytes)
		throw ImageInspectionError(ErrorCode::Capacity,
								   "Native export budget exhausted (eight tickets, 512 MiB payload)");
	for (auto& slot : state->charges)
		if (slot.expired())
		{
			charge->charged = true;
			state->bytes += bytes;
			++state->count;
			slot = charge;
			return charge;
		}
	throw ImageInspectionError(ErrorCode::Capacity, "Native export retirement capacity exhausted");
}
class VulkanControlDelivery final : public ImageDelivery
{
  public:
	explicit VulkanControlDelivery(std::shared_ptr<VulkanImageDelivery> native, std::vector<FrameView> views)
		: m_native(std::move(native)), m_views(std::move(views))
	{
	}
	SharedImageKey Key() const noexcept override
	{
		return m_native->Key();
	}
	ImageDescription Description() const override
	{
		auto result = Describe(m_native->Descriptor());
		result.views = m_views;
		result.completionKind = m_native->CompletionKind();
		return result;
	}
	CompletionStatus ProducerCompletion() const noexcept override
	{
		return m_native->PollCompletion();
	}
	CompletionStatus ConsumerCompletion() const noexcept override
	{
		return m_native->PollConsumerCompletion();
	}
	std::vector<std::shared_ptr<TransferHandle>> PrepareHandles() override
	{
		return Translate(
			[&]
			{
				auto handles = m_native->ExportHandles(false);
				std::vector<std::shared_ptr<TransferHandle>> result;
				result.reserve(2);
				result.push_back(OwnVulkanControlHandle(std::move(handles.memory)));
				result.push_back(OwnVulkanControlHandle(std::move(handles.ready)));
				return result;
			});
	}
	void MarkTransferred() noexcept override
	{
		m_native->MarkExposed();
	}

  private:
	const std::shared_ptr<VulkanImageDelivery> m_native;
	const std::vector<FrameView> m_views;
};
class VulkanImageExport final : public ImageExport
{
  public:
	VulkanImageExport(std::shared_ptr<VulkanImageReadback> native, ImageDescription description, std::uint64_t bytes,
					  std::optional<std::uint32_t> layer, std::optional<ImageRegion> region)
		: m_native(std::move(native)), m_description(std::move(description)), m_bytes(bytes), m_layer(layer), m_region(region)
	{
	}
	std::optional<ImageRegion> Region() const noexcept override
	{
		return m_region;
	}
	std::optional<std::uint32_t> SourceLayer() const noexcept override
	{
		return m_layer;
	}
	ImageDescription Description() const override
	{
		return m_description;
	}
	std::uint64_t ByteSize() const noexcept override
	{
		return m_bytes;
	}
	CompletionStatus PollCompletion() const noexcept override
	{
		return m_native->PollCompletion();
	}
	std::span<const std::byte> Read() override
	{
		return Translate(
			[&]() -> std::span<const std::byte>
			{
				const auto pixels = m_native->Pixels();
				return pixels ? std::as_bytes(std::span(pixels->rgba)) : std::span<const std::byte>{};
			});
	}

  private:
	const std::shared_ptr<VulkanImageReadback> m_native;
	const ImageDescription m_description;
	const std::uint64_t m_bytes;
	const std::optional<std::uint32_t> m_layer;
	const std::optional<ImageRegion> m_region;
};
class VulkanImageReader final : public ImageReader
{
  public:
	explicit VulkanImageReader(std::shared_ptr<const VulkanInspectionFrame> frame)
		: m_frame(std::move(frame)), m_description(Describe(*m_frame))
	{
	}
	ImageDescription Description() const override
	{
		return m_description;
	}
	ImageCapabilities Capabilities() const noexcept override
	{
		const bool live = !m_frame->Image().device->lost.load();
		return {live && m_frame->CanShare(), live};
	}
	CompletionStatus PollCompletion() const noexcept override
	{
		return m_frame->PollCompletion();
	}
	std::shared_ptr<ImageDelivery> Share(const ImageDeviceIdentity& identity,
										 const std::shared_ptr<TransferHandle>& consumer) const override
	{
		return Translate(
			[&]() -> std::shared_ptr<ImageDelivery>
			{
				auto fd = DuplicateVulkanControlHandle(consumer);
				auto delivery = m_frame->Share();
				delivery->RegisterConsumer(Identity(identity), fd);
				return std::make_shared<VulkanControlDelivery>(std::move(delivery), m_description.views);
			});
	}
	std::shared_ptr<ImageExport> Export(std::optional<std::uint32_t> layer) const override
	{
		return ExportSelected(layer, {});
	}
	std::shared_ptr<ImageExport> ExportRegion(std::optional<std::uint32_t> layer, const ImageRegion& region) const override
	{
		return ExportSelected(layer, region);
	}

  private:
	std::shared_ptr<ImageExport> ExportSelected(std::optional<std::uint32_t> layer, std::optional<ImageRegion> region) const
	{
		return Translate(
			[&]() -> std::shared_ptr<ImageExport>
			{
				auto description = m_description;
				if (layer && *layer >= description.arrayLayers)
					throw std::invalid_argument("Invalid export array slice");
				if (layer)
				{
					description.arrayLayers = 1;
					if (!description.views.empty())
						description.views = {description.views.at(*layer)};
				}
				bool resize = false;
				if (region)
				{
					const auto& selected = *region;
					if (!selected.width || !selected.height || !selected.outputWidth || !selected.outputHeight ||
						selected.x > description.width || selected.y > description.height ||
						selected.width > description.width - selected.x || selected.height > description.height - selected.y ||
						description.width > INT32_MAX || description.height > INT32_MAX ||
						selected.outputWidth > INT32_MAX || selected.outputHeight > INT32_MAX)
						throw std::invalid_argument("Invalid image export region");
					resize = selected.width != selected.outputWidth || selected.height != selected.outputHeight;
					description.width = selected.outputWidth;
					description.height = selected.outputHeight;
					description.views.clear();
				}
				const auto pixelBytes = ColorPixelBytes(description.format);
				if (!pixelBytes)
					throw ImageInspectionError(ErrorCode::UnsupportedCommand,
											   "Unsupported explicit color export format");
				constexpr std::uint64_t limit = 512ULL * 1024 * 1024;
				if (!description.width || !description.height || !description.arrayLayers ||
					description.width > limit / pixelBytes / description.height / description.arrayLayers)
					throw ImageInspectionError(ErrorCode::Capacity, "Image exceeds explicit export byte limit");
				const auto bytes =
					std::uint64_t{description.width} * description.height * description.arrayLayers * pixelBytes;
				const auto scratchLimit = resize ? std::max<std::uint64_t>(bytes * 2, 4ULL * 1024 * 1024) : 0;
				if (scratchLimit > limit - bytes)
					throw ImageInspectionError(ErrorCode::Capacity, "Resized export exceeds bounded byte capacity");
				auto charge = ReserveExport(bytes + scratchLimit, m_frame->Collector());
				auto native = m_frame->Export(layer, std::move(charge), region);
				return std::make_shared<VulkanImageExport>(std::move(native), std::move(description), bytes, layer, region);
			});
	}

	const std::shared_ptr<const VulkanInspectionFrame> m_frame;
	const ImageDescription m_description;
};
class VulkanImageInspectionProvider final : public ImageInspectionProvider
{
  public:
	explicit VulkanImageInspectionProvider(ImageCapabilityQuery query) : m_query(std::move(query)) {}
	ImageCapabilities Capabilities(XrSession session) const noexcept override
	{
		try
		{
			return m_query ? m_query(session) : ImageCapabilities{};
		}
		catch (...)
		{
			return {};
		}
	}
	std::shared_ptr<ImageReader> Retain(std::shared_ptr<const FrameOutput> output) const override
	{
		const auto frame = std::dynamic_pointer_cast<const VulkanInspectionFrame>(std::move(output));
		if (!frame)
			throw ImageInspectionError(ErrorCode::UnsupportedCommand,
									   "This output has no owned Vulkan inspection image");
		return std::make_shared<VulkanImageReader>(frame);
	}

  private:
	const ImageCapabilityQuery m_query;
};
std::shared_ptr<ImageInspectionProvider> CreateVulkanImageInspectionProvider(ImageCapabilityQuery query)
{
	(void)ExportBudget();
	return std::make_shared<VulkanImageInspectionProvider>(std::move(query));
}
} // namespace simxr::control
