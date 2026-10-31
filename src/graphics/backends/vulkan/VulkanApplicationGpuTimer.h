#pragma once
#include "graphics/ApplicationGpuTimer.h"
#include "graphics/backends/vulkan/VulkanDevice.h"

namespace simxr
{
std::unique_ptr<ApplicationGpuTimer> CreateVulkanApplicationGpuTimer(std::shared_ptr<VulkanDevice> device);
}
