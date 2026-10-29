#pragma once

#include "graphics/backends/vulkan/VulkanDevice.h"

#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <vector>

namespace simxr
{
// Scratch storage is leased exclusively and reset only after its prior GPU use finishes.
class VulkanCommands
{
public:
	explicit VulkanCommands(VkDevice device, std::uint32_t queueFamily);
	~VulkanCommands();
	VulkanCommands(const VulkanCommands&) = delete;
	VulkanCommands& operator=(const VulkanCommands&) = delete;
	void Reset();
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkQueryPool timestamps = VK_NULL_HANDLE;
	VkDescriptorPool descriptors = VK_NULL_HANDLE;

private:
	void Destroy() noexcept;
	const VkDevice m_device;
};

class VulkanImage
{
public:
	static std::shared_ptr<VulkanImage>
	Create(std::shared_ptr<VulkanDevice> device, VkFormat format, VkExtent2D extent, std::uint32_t layers,
		   VkImageUsageFlags usage, std::uint32_t mipLevels = 1, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT,
		   VkDeviceSize allocationLimit = VK_WHOLE_SIZE, VkImageCreateFlags flags = 0);
	~VulkanImage();
	VulkanImage(const VulkanImage&) = delete;
	VulkanImage& operator=(const VulkanImage&) = delete;
	const std::shared_ptr<VulkanDevice> device;
	VkImage image = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize allocationSize = 0;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkExtent2D extent{};
	std::uint32_t layers = 0;
	std::uint32_t mipLevels = 1;
	VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT;
	VkImageUsageFlags usage = 0;
	// Set before publication; observes the end of this image lease after its GPU uses retire.
	std::function<void()> retired;
	VkImageAspectFlags Aspect() const;
	VkImageLayout ApplicationLayout() const;
	// Single-slice views and framebuffers are cached on the allocation owner and destroyed with it,
	// after every lease and the GPU uses those leases retain.
	VkImageView View(std::uint32_t slice, VkImageAspectFlags aspect = VK_IMAGE_ASPECT_COLOR_BIT) const;
	VkFramebuffer Framebuffer(VkRenderPass pass, std::uint32_t slice) const;

private:
	friend class VulkanExternalDevice;
	friend class WindowsVulkanImport;
	friend class VulkanImagePool;
	friend class VulkanBackend;
	// A fresh image lifetime and retirement observer over a retained allocation.
	static std::shared_ptr<VulkanImage> Lease(std::shared_ptr<VulkanImage> allocation);
	explicit VulkanImage(std::shared_ptr<VulkanDevice> device);
	const VulkanImage& Owner() const noexcept;
	std::shared_ptr<VulkanImage> m_allocation;

	struct CachedView
	{
		std::uint32_t slice;
		VkImageAspectFlags aspect;
		VkImageView view;
	};

	struct CachedFramebuffer
	{
		VkRenderPass pass;
		std::uint32_t slice;
		VkFramebuffer framebuffer;
	};

	mutable std::mutex m_cacheMutex;
	mutable std::vector<CachedView> m_views;
	mutable std::vector<CachedFramebuffer> m_framebuffers;
};

void VulkanImageBarrier(VkCommandBuffer command, const VulkanImage& image, VkImageLayout previous, VkImageLayout next,
						VkAccessFlags sourceAccess, VkAccessFlags destinationAccess);
} // namespace simxr
