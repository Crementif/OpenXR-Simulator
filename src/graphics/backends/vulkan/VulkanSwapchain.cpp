#include "graphics/backends/vulkan/VulkanSwapchain.h"

#include "Pch.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

namespace simxr
{
static bool DepthFormat(VkFormat format)
{
	return format == VK_FORMAT_D32_SFLOAT || format == VK_FORMAT_D24_UNORM_S8_UINT || format == VK_FORMAT_D16_UNORM ||
		   format == VK_FORMAT_D32_SFLOAT_S8_UINT;
}

static XrResult ValidateDescription(const XrSwapchainCreateInfo& description, const VulkanDevice& device)
{
	if (description.type != XR_TYPE_SWAPCHAIN_CREATE_INFO || !description.width || !description.height ||
		!description.arraySize || !description.sampleCount || !description.mipCount || !description.usageFlags ||
		(description.faceCount != 1 && description.faceCount != 6))
		return XR_ERROR_VALIDATION_FAILURE;
	constexpr XrSwapchainUsageFlags SupportedUsage =
		XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT | XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT |
		XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT | XR_SWAPCHAIN_USAGE_TRANSFER_SRC_BIT |
		XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_SAMPLED_BIT | XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT;
	if ((description.createFlags & ~XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) ||
		(description.usageFlags & ~SupportedUsage) || description.faceCount != 1)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	if (!std::has_single_bit(description.sampleCount) || description.sampleCount > 64 ||
		(description.sampleCount != 1 && description.mipCount != 1))
		return XR_ERROR_VALIDATION_FAILURE;
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(device.binding.physicalDevice, &properties);
	if (description.width > properties.limits.maxImageDimension2D ||
		description.height > properties.limits.maxImageDimension2D ||
		description.arraySize > properties.limits.maxImageArrayLayers ||
		description.mipCount >
			static_cast<std::uint32_t>(std::bit_width(std::max(description.width, description.height))))
		return XR_ERROR_VALIDATION_FAILURE;
	const auto formats = VulkanSwapchain::Formats();
	if (std::find(formats.begin(), formats.end(), description.format) == formats.end())
		return XR_ERROR_SWAPCHAIN_FORMAT_UNSUPPORTED;
	return XR_SUCCESS;
}

static VkImageUsageFlags ImageUsage(const XrSwapchainCreateInfo& description)
{
	VkImageUsageFlags usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (description.usageFlags & XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT)
		usage |= VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (description.usageFlags & XR_SWAPCHAIN_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT)
		usage |= VK_IMAGE_USAGE_DEPTH_STENCIL_ATTACHMENT_BIT;
	if (description.usageFlags & XR_SWAPCHAIN_USAGE_UNORDERED_ACCESS_BIT)
		usage |= VK_IMAGE_USAGE_STORAGE_BIT;
	if (!DepthFormat(static_cast<VkFormat>(description.format)) ||
		(description.usageFlags & XR_SWAPCHAIN_USAGE_SAMPLED_BIT))
		usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	return usage;
}

VulkanSwapchain::VulkanSwapchain(XrSwapchainCreateInfo description)
	: m_description(description), m_images(description.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT ? 1 : 3),
	  m_access(static_cast<std::uint32_t>(m_images.size()),
			   (description.createFlags & XR_SWAPCHAIN_CREATE_STATIC_IMAGE_BIT) != 0)
{}

std::span<const std::int64_t> VulkanSwapchain::Formats()
{
	static constexpr std::array<std::int64_t, 11> s_formats{VK_FORMAT_R8G8B8A8_SRGB,
															VK_FORMAT_R8G8B8A8_UNORM,
															VK_FORMAT_B8G8R8A8_SRGB,
															VK_FORMAT_B8G8R8A8_UNORM,
															VK_FORMAT_R16G16B16A16_SFLOAT,
															VK_FORMAT_R32G32B32A32_SFLOAT,
															VK_FORMAT_A2B10G10R10_UNORM_PACK32,
															VK_FORMAT_D32_SFLOAT,
															VK_FORMAT_D24_UNORM_S8_UINT,
															VK_FORMAT_D16_UNORM,
															VK_FORMAT_D32_SFLOAT_S8_UINT};
	return s_formats;
}

std::span<const std::int64_t> VulkanSwapchain::ColorFormats()
{
	return Formats().first(7);
}

XrResult VulkanSwapchain::Create(VulkanBackend& backend, const XrSwapchainCreateInfo& description,
								 std::unique_ptr<VulkanSwapchain>& output)
{
	const auto validation = ValidateDescription(description, *backend.Device());
	if (XR_FAILED(validation))
		return validation;
	auto copied = description;
	copied.next = nullptr;
	auto prepared = std::unique_ptr<VulkanSwapchain>(new VulkanSwapchain(copied));
	auto usage = ImageUsage(description);
	const VkImageCreateFlags flags =
		description.usageFlags & XR_SWAPCHAIN_USAGE_MUTABLE_FORMAT_BIT ? VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT : 0;
	if (DepthFormat(static_cast<VkFormat>(description.format)) && !(usage & VK_IMAGE_USAGE_SAMPLED_BIT))
	{
		VkImageFormatProperties properties{};
		if (vkGetPhysicalDeviceImageFormatProperties(
				backend.Device()->binding.physicalDevice, static_cast<VkFormat>(description.format), VK_IMAGE_TYPE_2D,
				VK_IMAGE_TILING_OPTIMAL, usage | VK_IMAGE_USAGE_SAMPLED_BIT, flags, &properties) == VK_SUCCESS &&
			description.width <= properties.maxExtent.width && description.height <= properties.maxExtent.height &&
			description.arraySize <= properties.maxArrayLayers && description.mipCount <= properties.maxMipLevels &&
			(properties.sampleCounts & description.sampleCount))
			usage |= VK_IMAGE_USAGE_SAMPLED_BIT;
	}
	for (auto& image : prepared->m_images)
		image.resource = backend.CreateImage(
			static_cast<VkFormat>(description.format), {description.width, description.height}, description.arraySize,
			usage, description.mipCount, static_cast<VkSampleCountFlagBits>(description.sampleCount), flags);
	output = std::move(prepared);
	return XR_SUCCESS;
}

XrResult VulkanSwapchain::EnumerateImages(std::uint32_t capacity, std::uint32_t* count,
										  XrSwapchainImageBaseHeader* images) const
{
	if (!count || (capacity && !images))
		return XR_ERROR_VALIDATION_FAILURE;
	if (m_images.empty())
		return XR_ERROR_SESSION_LOST;
	*count = static_cast<std::uint32_t>(m_images.size());
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	auto* native = reinterpret_cast<XrSwapchainImageVulkanKHR*>(images);
	for (std::size_t index = 0; index < m_images.size(); ++index)
		if (native[index].type != XR_TYPE_SWAPCHAIN_IMAGE_VULKAN_KHR)
			return XR_ERROR_VALIDATION_FAILURE;
	for (std::size_t index = 0; index < m_images.size(); ++index)
		native[index].image = m_images[index].resource->image;
	return XR_SUCCESS;
}

XrResult VulkanSwapchain::Acquire(VulkanBackend& backend, std::uint32_t& index)
{
	if (m_images.empty())
		return XR_ERROR_SESSION_LOST;
	if (m_images.front().resource->device != backend.Device())
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	return m_access.Acquire(index, [&](std::uint32_t candidate) -> std::shared_ptr<const GpuCompletion> {
		auto& image = m_images[candidate];
		if (image.initialized)
			return {};
		auto initialized = backend.InitializeImage(image.resource);
		image.initialized = true;
		return initialized;
	});
}

XrResult VulkanSwapchain::Wait(XrDuration timeout)
{
	return m_access.Wait(timeout);
}

XrResult VulkanSwapchain::Release()
{
	return m_access.Release();
}

SwapchainDescription VulkanSwapchain::Description() const
{
	return {m_description.width, m_description.height, m_description.arraySize, m_access.ReleasedImage().has_value()};
}

std::shared_ptr<VulkanImage> VulkanSwapchain::ReleasedImage() const
{
	const auto index = m_access.ReleasedImage();
	return index ? m_images[*index].resource : nullptr;
}

void VulkanSwapchain::MarkReleasedUsed(const std::shared_ptr<const GpuCompletion>& completion)
{
	m_access.MarkReleasedUsed(completion);
}

void VulkanSwapchain::Close()
{
	m_access.Close();
	m_images.clear();
}
} // namespace simxr
