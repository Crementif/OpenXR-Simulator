#include "graphics/backends/vulkan/VulkanDevice.h"

#include <string>
#include <vector>

namespace simxr
{
VulkanError::VulkanError(VkResult result, const char* operation)
	: std::runtime_error(std::string(operation) + ": Vulkan result " + std::to_string(result)), m_result(result)
{
}

VkResult VulkanError::Result() const noexcept
{
	return m_result;
}

void CheckVulkan(VkResult result, const char* operation)
{
	if (result != VK_SUCCESS)
		throw VulkanError(result, operation);
}

VulkanDevice::VulkanDevice(const VulkanBinding& value, std::shared_ptr<const void> owner)
	: binding(value), m_owner(std::move(owner))
{
	if (!binding.instance || !binding.physicalDevice || !binding.device)
		throw std::invalid_argument("Vulkan binding requires instance, physical device and device");
	std::uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(binding.physicalDevice, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(binding.physicalDevice, &count, families.data());
	if (binding.queueFamily >= count || binding.queueIndex >= families[binding.queueFamily].queueCount ||
		!(families[binding.queueFamily].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		throw std::invalid_argument("Vulkan binding requires a valid graphics queue");
	vkGetDeviceQueue(binding.device, binding.queueFamily, binding.queueIndex, &queue);
	if (!queue)
		throw std::invalid_argument("Vulkan binding queue was not enabled by the application");
}

bool VulkanDevice::HasNativeOwner() const noexcept
{
	return static_cast<bool>(m_owner);
}

std::uint32_t VulkanDevice::MemoryType(std::uint32_t bits, VkMemoryPropertyFlags required) const
{
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(binding.physicalDevice, &properties);
	for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
		if ((bits & (1u << index)) && (properties.memoryTypes[index].propertyFlags & required) == required)
			return index;
	throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "No compatible Vulkan memory type");
}
} // namespace simxr
