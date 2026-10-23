#pragma once

#include "graphics/backends/d3d12/D3D12Backend.h"
#include "graphics/backends/windows/WindowsVulkanImport.h"

#include <memory>

namespace simxr
{
std::shared_ptr<WindowsVulkanImport> ImportD3D12Frame(std::shared_ptr<VulkanDevice> device,
													  std::shared_ptr<D3D12Frame> frame);
} // namespace simxr
