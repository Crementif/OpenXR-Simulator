#pragma once

#include "graphics/backends/vulkan/VulkanDevice.h"

#include <cstdint>
#include <openxr/openxr.h>
#include <optional>
#include <string>
#include <vector>

struct XrGraphicsRequirementsVulkanKHR;
struct XrVulkanInstanceCreateInfoKHR;
struct XrVulkanDeviceCreateInfoKHR;

namespace simxr
{
XrResult VulkanGraphicsFailure(VkResult result);
std::vector<const char*> VulkanSharingExtensions(VkPhysicalDevice physicalDevice);

// Externally serialized by the native instance gate. Application Vulkan handles
// are borrowed; selection records associate physical devices with their actual
// instance for application-provided dispatch. No application handle is destroyed.
class VulkanInstanceGraphics final
{
  public:
	VulkanInstanceGraphics() = default;
	VulkanInstanceGraphics(const VulkanInstanceGraphics&) = delete;
	VulkanInstanceGraphics& operator=(const VulkanInstanceGraphics&) = delete;
	XrResult Requirements(XrSystemId system, XrGraphicsRequirementsVulkanKHR* output);
	XrResult InstanceExtensions(XrSystemId system, std::uint32_t capacity, std::uint32_t* count, char* output) const;
	XrResult DeviceExtensions(XrSystemId system, std::uint32_t capacity, std::uint32_t* count, char* output);
	XrResult PhysicalDevice(XrSystemId system, VkInstance instance, VkPhysicalDevice* output);
	XrResult CreateInstance(const XrVulkanInstanceCreateInfoKHR* info, VkInstance* output, VkResult* result) const;
	XrResult CreateDevice(const XrVulkanDeviceCreateInfoKHR* info, VkDevice* output, VkResult* result) const;
	XrResult SessionBinding(const XrSessionCreateInfo& info, VulkanBinding& output) const;

  private:
	struct Selection
	{
		VkPhysicalDevice physicalDevice;
		VkInstance instance;
	};

	std::optional<Selection> FindSelection(VkPhysicalDevice device) const;
	std::optional<std::string> m_deviceExtensions;
	std::vector<Selection> m_selections;
	bool m_requirementsRead = false;
};
} // namespace simxr
