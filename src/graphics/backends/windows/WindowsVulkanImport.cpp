#include "graphics/backends/windows/WindowsVulkanImport.h"

#include <dxgi.h>
#include <vulkan/vulkan_win32.h>
#include <cstring>

namespace simxr
{
WindowsVulkanImport::WindowsVulkanImport(std::shared_ptr<VulkanDevice> device, std::shared_ptr<const FrameOutput> owner)
	: m_device(std::move(device)), m_owner(std::move(owner))
{
}

WindowsVulkanImport::~WindowsVulkanImport()
{
	if (m_ready)
		vkDestroySemaphore(m_device->binding.device, m_ready, nullptr);
}

const std::shared_ptr<VulkanImage>& WindowsVulkanImport::Image() const noexcept
{
	return m_image;
}
VkSemaphore WindowsVulkanImport::Ready() const noexcept
{
	return m_ready;
}

std::shared_ptr<WindowsVulkanImport> WindowsVulkanImport::Create(std::shared_ptr<VulkanDevice> device,
	const WindowsSharedImage& source, std::shared_ptr<const FrameOutput> owner)
{
	if (!device || !owner || !source.texture || !source.fence || !source.width || !source.height || !source.layers || !source.mipLevels)
		throw std::invalid_argument("Windows native import requires a device and frame");
	const auto native = device->binding.device;
	const auto memoryProperties = reinterpret_cast<PFN_vkGetMemoryWin32HandlePropertiesKHR>(
		vkGetDeviceProcAddr(native, "vkGetMemoryWin32HandlePropertiesKHR"));
	const auto importSemaphore = reinterpret_cast<PFN_vkImportSemaphoreWin32HandleKHR>(
		vkGetDeviceProcAddr(native, "vkImportSemaphoreWin32HandleKHR"));
	if (!memoryProperties || !importSemaphore)
		throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Windows native external import extensions not enabled");
	VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};
	vkGetPhysicalDeviceProperties2(device->binding.physicalDevice, &properties);
	if (!identity.deviceLUIDValid || std::memcmp(identity.deviceLUID, &source.adapter, VK_LUID_SIZE))
		throw VulkanError(VK_ERROR_INVALID_EXTERNAL_HANDLE, "Windows native and Vulkan adapter LUIDs differ");

	const auto memoryType = source.handleType;
	constexpr auto semaphoreType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_D3D12_FENCE_BIT;
	VkPhysicalDeviceExternalImageFormatInfo externalFormat{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
	externalFormat.handleType = memoryType;
	VkPhysicalDeviceImageFormatInfo2 format{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2, &externalFormat};
	switch (source.format)
	{
	case DXGI_FORMAT_R8G8B8A8_UNORM: format.format = VK_FORMAT_R8G8B8A8_UNORM; break;
	case DXGI_FORMAT_R8G8B8A8_UNORM_SRGB: format.format = VK_FORMAT_R8G8B8A8_SRGB; break;
	case DXGI_FORMAT_B8G8R8A8_UNORM: format.format = VK_FORMAT_B8G8R8A8_UNORM; break;
	case DXGI_FORMAT_B8G8R8A8_UNORM_SRGB: format.format = VK_FORMAT_B8G8R8A8_SRGB; break;
	case DXGI_FORMAT_R16G16B16A16_FLOAT: format.format = VK_FORMAT_R16G16B16A16_SFLOAT; break;
	case DXGI_FORMAT_R10G10B10A2_UNORM: format.format = VK_FORMAT_A2B10G10R10_UNORM_PACK32; break;
	default: throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED, "Unsupported Windows native shared image format");
	}
	format.type = VK_IMAGE_TYPE_2D;
	format.tiling = VK_IMAGE_TILING_OPTIMAL;
	format.usage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
	VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 supported{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2, &externalProperties};
	CheckVulkan(vkGetPhysicalDeviceImageFormatProperties2(device->binding.physicalDevice, &format, &supported),
		"Query Windows native shared texture import");
	if (!(externalProperties.externalMemoryProperties.externalMemoryFeatures & VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT) ||
		source.width > supported.imageFormatProperties.maxExtent.width ||
		source.height > supported.imageFormatProperties.maxExtent.height ||
		source.layers > supported.imageFormatProperties.maxArrayLayers ||
		source.mipLevels > supported.imageFormatProperties.maxMipLevels)
		throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED, "Windows native shared texture import unsupported");
	VkSemaphoreTypeCreateInfo type{VK_STRUCTURE_TYPE_SEMAPHORE_TYPE_CREATE_INFO};
	type.semaphoreType = VK_SEMAPHORE_TYPE_TIMELINE;
	VkPhysicalDeviceExternalSemaphoreInfo semaphoreInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO, &type};
	semaphoreInfo.handleType = semaphoreType;
	VkExternalSemaphoreProperties semaphoreProperties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
	vkGetPhysicalDeviceExternalSemaphoreProperties(device->binding.physicalDevice, &semaphoreInfo, &semaphoreProperties);
	if (!(semaphoreProperties.externalSemaphoreFeatures & VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT))
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Windows native shared fence import unsupported");

	auto result = std::shared_ptr<WindowsVulkanImport>(new WindowsVulkanImport(device, std::move(owner)));
	auto image = std::shared_ptr<VulkanImage>(new VulkanImage(device));
	image->format = format.format;
	image->extent = {source.width, source.height};
	image->layers = source.layers;
	image->mipLevels = source.mipLevels;
	image->usage = format.usage;
	VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	external.handleTypes = memoryType;
	VkImageCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &external};
	create.imageType = format.type;
	create.format = format.format;
	create.extent = {source.width, source.height, 1};
	create.mipLevels = source.mipLevels;
	create.arrayLayers = source.layers;
	create.samples = VK_SAMPLE_COUNT_1_BIT;
	create.tiling = format.tiling;
	create.usage = format.usage;
	CheckVulkan(vkCreateImage(native, &create, nullptr, &image->image), "Create Windows native import image");
	VkMemoryRequirements requirements;
	vkGetImageMemoryRequirements(native, image->image, &requirements);
	VkMemoryWin32HandlePropertiesKHR handleProperties{VK_STRUCTURE_TYPE_MEMORY_WIN32_HANDLE_PROPERTIES_KHR};
	CheckVulkan(memoryProperties(native, memoryType, source.texture, &handleProperties),
		"Query Windows native shared memory types");
	VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dedicated.image = image->image;
	VkImportMemoryWin32HandleInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_WIN32_HANDLE_INFO_KHR, &dedicated};
	imported.handleType = memoryType;
	imported.handle = source.texture;
	VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &imported};
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = device->MemoryType(requirements.memoryTypeBits & handleProperties.memoryTypeBits, 0);
	CheckVulkan(vkAllocateMemory(native, &allocation, nullptr, &image->memory), "Import Windows native shared texture memory");
	image->allocationSize = requirements.size;
	CheckVulkan(vkBindImageMemory(native, image->image, image->memory, 0), "Bind Windows native shared texture memory");
	result->m_image = std::move(image);
	VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO, &type};
	CheckVulkan(vkCreateSemaphore(native, &semaphore, nullptr, &result->m_ready), "Create Windows native fence semaphore");
	VkImportSemaphoreWin32HandleInfoKHR fence{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_WIN32_HANDLE_INFO_KHR};
	fence.semaphore = result->m_ready;
	fence.handleType = semaphoreType;
	fence.handle = source.fence;
	CheckVulkan(importSemaphore(native, &fence), "Import Windows native producer fence");
	return result;
}
}
