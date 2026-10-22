#include "graphics/backends/vulkan/VulkanResources.h"

#include <algorithm>
#include <bit>
#include <utility>

namespace simxr
{
VulkanImage::VulkanImage(std::shared_ptr<VulkanDevice> owner) : device(std::move(owner)) {}

std::shared_ptr<VulkanImage> VulkanImage::Create(std::shared_ptr<VulkanDevice> device, VkFormat format,
												 VkExtent2D extent, std::uint32_t layers, VkImageUsageFlags usage, std::uint32_t mipLevels)
{
	if (!device || !extent.width || !extent.height || !layers || !usage || !mipLevels ||
		mipLevels > std::bit_width(std::max(extent.width, extent.height)))
		throw std::invalid_argument("Vulkan image requires device, extent, layers and usage");
	VkImageFormatProperties supported{};
	CheckVulkan(vkGetPhysicalDeviceImageFormatProperties(device->binding.physicalDevice, format, VK_IMAGE_TYPE_2D,
														 VK_IMAGE_TILING_OPTIMAL, usage, 0, &supported),
				"Query Vulkan image format and usage");
	if (extent.width > supported.maxExtent.width || extent.height > supported.maxExtent.height ||
		layers > supported.maxArrayLayers || mipLevels > supported.maxMipLevels ||
		!(supported.sampleCounts & VK_SAMPLE_COUNT_1_BIT))
		throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED, "Vulkan image extent or layers unsupported");
	auto result = std::shared_ptr<VulkanImage>(new VulkanImage(std::move(device)));
	result->format = format;
	result->extent = extent;
	result->layers = layers;
	result->mipLevels = mipLevels;
	result->usage = usage;
	VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = format;
	image.extent = {extent.width, extent.height, 1};
	image.mipLevels = mipLevels;
	image.arrayLayers = layers;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = usage;
	CheckVulkan(vkCreateImage(result->device->binding.device, &image, nullptr, &result->image),
				"Create private Vulkan image");
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(result->device->binding.device, result->image, &requirements);
	VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocation.allocationSize = requirements.size;
	result->allocationSize = requirements.size;
	try
	{
		allocation.memoryTypeIndex =
			result->device->MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	}
	catch (const VulkanError&)
	{
		allocation.memoryTypeIndex = result->device->MemoryType(requirements.memoryTypeBits, 0);
	}
	CheckVulkan(vkAllocateMemory(result->device->binding.device, &allocation, nullptr, &result->memory),
				"Allocate Vulkan image memory");
	CheckVulkan(vkBindImageMemory(result->device->binding.device, result->image, result->memory, 0),
				"Bind Vulkan image memory");
	return result;
}

VulkanImage::~VulkanImage()
{
	if (image)
		vkDestroyImage(device->binding.device, image, nullptr);
	if (memory)
		vkFreeMemory(device->binding.device, memory, nullptr);
	try
	{
		if (retired)
			retired();
	}
	catch (...)
	{
		// Observers cannot alter resource retirement.
	}
}

VkImageAspectFlags VulkanImage::Aspect() const
{
	switch (format)
	{
	case VK_FORMAT_D16_UNORM:
	case VK_FORMAT_D32_SFLOAT:
		return VK_IMAGE_ASPECT_DEPTH_BIT;
	case VK_FORMAT_D24_UNORM_S8_UINT:
	case VK_FORMAT_D32_SFLOAT_S8_UINT:
		return VK_IMAGE_ASPECT_DEPTH_BIT | VK_IMAGE_ASPECT_STENCIL_BIT;
	default:
		return VK_IMAGE_ASPECT_COLOR_BIT;
	}
}

VkImageLayout VulkanImage::ApplicationLayout() const
{
	return (Aspect() & VK_IMAGE_ASPECT_DEPTH_BIT) ? VK_IMAGE_LAYOUT_DEPTH_STENCIL_ATTACHMENT_OPTIMAL
												  : VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
}

void VulkanImageBarrier(VkCommandBuffer command, const VulkanImage& image, VkImageLayout previous, VkImageLayout next,
						VkAccessFlags sourceAccess, VkAccessFlags destinationAccess)
{
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.srcAccessMask = sourceAccess;
	barrier.dstAccessMask = destinationAccess;
	barrier.oldLayout = previous;
	barrier.newLayout = next;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.image = image.image;
	barrier.subresourceRange = {image.Aspect(), 0, image.mipLevels, 0, image.layers};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
						 0, nullptr, 1, &barrier);
}
} // namespace simxr
