#pragma once

#include <openxr/openxr.h>
#include <vulkan/vulkan.h>

namespace simxr::probe
{
bool VerifyVulkanCreation(XrInstance instance, XrSystemId system, VkInstance graphicsInstance,
						  VkPhysicalDevice physicalDevice);
} // namespace simxr::probe
