#pragma once

#include "graphics/backends/d3d11/D3D11Backend.h"
#include "graphics/backends/windows/WindowsVulkanImport.h"

#include <memory>

namespace simxr
{
std::shared_ptr<WindowsVulkanImport> ImportD3D11Frame(std::shared_ptr<VulkanDevice> device,
													  std::shared_ptr<D3D11Frame> frame);
} // namespace simxr
