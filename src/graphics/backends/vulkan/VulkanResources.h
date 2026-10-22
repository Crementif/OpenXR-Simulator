#pragma once

#include "graphics/backends/vulkan/VulkanDevice.h"

#include <functional>
#include <memory>

namespace simxr
{
class VulkanImage
{
  public:
	static std::shared_ptr<VulkanImage> Create(std::shared_ptr<VulkanDevice> device, VkFormat format, VkExtent2D extent,
											   std::uint32_t layers, VkImageUsageFlags usage, std::uint32_t mipLevels = 1, VkSampleCountFlagBits samples = VK_SAMPLE_COUNT_1_BIT);
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
	// Set before publication; observes actual native image destruction only.
	std::function<void()> retired;
	VkImageAspectFlags Aspect() const;
	VkImageLayout ApplicationLayout() const;

  private:
	friend class VulkanExternalDevice;
	explicit VulkanImage(std::shared_ptr<VulkanDevice> device);
};

void VulkanImageBarrier(VkCommandBuffer command, const VulkanImage& image, VkImageLayout previous, VkImageLayout next,
						VkAccessFlags sourceAccess, VkAccessFlags destinationAccess);
} // namespace simxr
