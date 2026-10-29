#pragma once

#include "graphics/FrameOutput.h"
#include "graphics/backends/vulkan/VulkanResources.h"
#include <cstdint>
#include <memory>

#include <dxgi.h>

namespace simxr
{
struct WindowsSharedImage
{
	LUID adapter{};
	HANDLE texture = nullptr, fence = nullptr;
	VkExternalMemoryHandleTypeFlagBits handleType{};
	DXGI_FORMAT format = DXGI_FORMAT_UNKNOWN;
	std::uint32_t width = 0, height = 0, layers = 0, mipLevels = 0;
};

// Retain this owner until the importing queue has completed use. The device must
// enable Win32 external memory, Win32 external semaphores and timeline semaphores.
class WindowsVulkanImport
{
public:
	static std::shared_ptr<WindowsVulkanImport> Create(std::shared_ptr<VulkanDevice> device,
													   const WindowsSharedImage& source,
													   std::shared_ptr<const FrameOutput> owner);
	~WindowsVulkanImport();
	const std::shared_ptr<VulkanImage>& Image() const noexcept;
	VkSemaphore Ready() const noexcept;

private:
	WindowsVulkanImport(std::shared_ptr<VulkanDevice> device, std::shared_ptr<const FrameOutput> owner);
	const std::shared_ptr<VulkanDevice> m_device;
	const std::shared_ptr<const FrameOutput> m_owner;
	std::shared_ptr<VulkanImage> m_image;
	VkSemaphore m_ready = VK_NULL_HANDLE;
};
} // namespace simxr
