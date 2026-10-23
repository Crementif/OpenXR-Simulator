#pragma once

#include <cstdint>
#include <memory>
#include <vulkan/vulkan.h>

namespace simxr::app
{
// Borrowed access to companion-owned resources; users must be destroyed first.
struct VulkanDeviceAccess
{
	VkInstance instance = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	std::uint32_t queueFamily = 0;
	VkQueue ioQueue = VK_NULL_HANDLE;
	bool externalImages = false;
	bool externalFence = false;
	// Native consumers retain the actual renderer device on unknown completion.
	std::shared_ptr<const void> owner;
	bool externalTimeline = false;
};
} // namespace simxr::app
