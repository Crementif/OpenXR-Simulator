#pragma once

#include <openxr/openxr.h>
#include <vulkan/vulkan.h>

bool VerifyVulkanCreation(XrInstance instance, XrSystemId system, VkInstance graphicsInstance,
						  VkPhysicalDevice physicalDevice);
