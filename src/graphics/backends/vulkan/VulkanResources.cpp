#include "graphics/backends/vulkan/VulkanResources.h"

#include "Pch.h"

namespace simxr
{
VulkanCommands::VulkanCommands(VkDevice device, std::uint32_t queueFamily) : m_device(device)
{
	try
	{
		VkCommandPoolCreateInfo info{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		info.queueFamilyIndex = queueFamily;
		CheckVulkan(vkCreateCommandPool(device, &info, nullptr, &pool), "Create reusable command pool");
		VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		allocate.commandPool = pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		CheckVulkan(vkAllocateCommandBuffers(device, &allocate, &command), "Allocate reusable commands");
		VkFenceCreateInfo completion{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		CheckVulkan(vkCreateFence(device, &completion, nullptr, &fence), "Create reusable completion fence");
	}
	catch (...)
	{
		Destroy();
		throw;
	}
}
VulkanCommands::~VulkanCommands()
{
	Destroy();
}
void VulkanCommands::Destroy() noexcept
{
	if (pool)
		vkDestroyCommandPool(m_device, pool, nullptr);
	if (descriptors)
		vkDestroyDescriptorPool(m_device, descriptors, nullptr);
	if (timestamps)
		vkDestroyQueryPool(m_device, timestamps, nullptr);
	if (fence)
		vkDestroyFence(m_device, fence, nullptr);
}
void VulkanCommands::Reset()
{
	CheckVulkan(vkResetCommandPool(m_device, pool, 0), "Reset completed command pool");
	if (descriptors)
		CheckVulkan(vkResetDescriptorPool(m_device, descriptors, 0), "Reset completed descriptors");
	CheckVulkan(vkResetFences(m_device, 1, &fence), "Reset completed fence");
}

VulkanImage::VulkanImage(std::shared_ptr<VulkanDevice> owner) : device(std::move(owner)) {}

std::shared_ptr<VulkanImage> VulkanImage::Create(std::shared_ptr<VulkanDevice> device, VkFormat format,
												 VkExtent2D extent, std::uint32_t layers, VkImageUsageFlags usage,
												 std::uint32_t mipLevels, VkSampleCountFlagBits samples,
												 VkDeviceSize allocationLimit, VkImageCreateFlags flags)
{
	if (!device || !extent.width || !extent.height || !layers || !usage || !mipLevels ||
		mipLevels > static_cast<std::uint32_t>(std::bit_width(std::max(extent.width, extent.height))) ||
		!std::has_single_bit(static_cast<std::uint32_t>(samples)) ||
		(samples != VK_SAMPLE_COUNT_1_BIT && mipLevels != 1))
		throw std::invalid_argument("Vulkan image requires device, extent, layers and usage");
	VkImageFormatProperties supported{};
	CheckVulkan(vkGetPhysicalDeviceImageFormatProperties(device->binding.physicalDevice, format, VK_IMAGE_TYPE_2D,
														 VK_IMAGE_TILING_OPTIMAL, usage, flags, &supported),
				"Query Vulkan image format and usage");
	if (extent.width > supported.maxExtent.width || extent.height > supported.maxExtent.height ||
		layers > supported.maxArrayLayers || mipLevels > supported.maxMipLevels || !(supported.sampleCounts & samples))
		throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED, "Vulkan image extent or layers unsupported");
	auto result = std::shared_ptr<VulkanImage>(new VulkanImage(std::move(device)));
	result->format = format;
	result->extent = extent;
	result->layers = layers;
	result->mipLevels = mipLevels;
	result->samples = samples;
	result->usage = usage;
	VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	image.flags = flags;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = format;
	image.extent = {extent.width, extent.height, 1};
	image.mipLevels = mipLevels;
	image.arrayLayers = layers;
	image.samples = samples;
	image.tiling = VK_IMAGE_TILING_OPTIMAL;
	image.usage = usage;
	CheckVulkan(vkCreateImage(result->device->binding.device, &image, nullptr, &result->image),
				"Create private Vulkan image");
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(result->device->binding.device, result->image, &requirements);
	if (requirements.size > allocationLimit)
		throw std::length_error("Vulkan image allocation exceeds its budget");
	// Each image owns its entire allocation; declare that ownership to the driver.
	VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dedicated.image = result->image;
	VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
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

std::shared_ptr<VulkanImage> VulkanImage::Lease(std::shared_ptr<VulkanImage> allocation)
{
	auto result = std::shared_ptr<VulkanImage>(new VulkanImage(allocation->device));
	result->m_allocation = std::move(allocation);
	const auto& source = *result->m_allocation;
	result->image = source.image;
	result->memory = source.memory;
	result->allocationSize = source.allocationSize;
	result->format = source.format;
	result->extent = source.extent;
	result->layers = source.layers;
	result->mipLevels = source.mipLevels;
	result->samples = source.samples;
	result->usage = source.usage;
	return result;
}

VulkanImage::~VulkanImage()
{
	for (const auto& cached : m_framebuffers)
		vkDestroyFramebuffer(device->binding.device, cached.framebuffer, nullptr);
	for (const auto& cached : m_views)
		vkDestroyImageView(device->binding.device, cached.view, nullptr);
	if (image && !m_allocation)
		vkDestroyImage(device->binding.device, image, nullptr);
	if (memory && !m_allocation)
		vkFreeMemory(device->binding.device, memory, nullptr);
	m_allocation.reset();
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

const VulkanImage& VulkanImage::Owner() const noexcept
{
	const auto* owner = this;
	while (owner->m_allocation)
		owner = owner->m_allocation.get();
	return *owner;
}

VkImageView VulkanImage::View(std::uint32_t slice, VkImageAspectFlags aspect) const
{
	const auto& owner = Owner();
	std::lock_guard lock(owner.m_cacheMutex);
	for (const auto& cached : owner.m_views)
		if (cached.slice == slice && cached.aspect == aspect)
			return cached.view;
	VkImageViewCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	info.image = owner.image;
	info.viewType = VK_IMAGE_VIEW_TYPE_2D;
	info.format = owner.format;
	info.subresourceRange = {aspect, 0, owner.mipLevels, slice, 1};
	owner.m_views.reserve(owner.m_views.size() + 1);
	VkImageView view;
	CheckVulkan(vkCreateImageView(owner.device->binding.device, &info, nullptr, &view), "Create cached image view");
	owner.m_views.push_back({slice, aspect, view});
	return view;
}

VkFramebuffer VulkanImage::Framebuffer(VkRenderPass pass, std::uint32_t slice) const
{
	const auto view = View(slice);
	const auto& owner = Owner();
	std::lock_guard lock(owner.m_cacheMutex);
	for (const auto& cached : owner.m_framebuffers)
		if (cached.pass == pass && cached.slice == slice)
			return cached.framebuffer;
	VkFramebufferCreateInfo info{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
	info.renderPass = pass;
	info.attachmentCount = 1;
	info.pAttachments = &view;
	info.width = owner.extent.width;
	info.height = owner.extent.height;
	info.layers = 1;
	owner.m_framebuffers.reserve(owner.m_framebuffers.size() + 1);
	VkFramebuffer framebuffer;
	CheckVulkan(vkCreateFramebuffer(owner.device->binding.device, &info, nullptr, &framebuffer),
				"Create cached framebuffer");
	owner.m_framebuffers.push_back({pass, slice, framebuffer});
	return framebuffer;
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
