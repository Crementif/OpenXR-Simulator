#pragma once

#include "VulkanProbeInstance.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>
#include <unistd.h>

namespace simxr::probe
{
constexpr VkFormat ImageFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkExtent3D ImageExtent{64, 64, 1};
constexpr VkImageUsageFlags ImageUsage = VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
	VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageSubresourceRange ColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
class FileDescriptor
{
public:
	explicit FileDescriptor(int value) : m_value(value) {}
	~FileDescriptor()
	{
		if (m_value >= 0)
			close(m_value);
	}
	FileDescriptor(const FileDescriptor&) = delete;
	FileDescriptor& operator=(const FileDescriptor&) = delete;
	void Release() { m_value = -1; }

private:
	int m_value;
};

class Device
{
public:
	Device() = default;
	Device(const Device&) = delete;
	Device& operator=(const Device&) = delete;

	VkDevice handle = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	std::uint32_t family = 0;

	~Device()
	{
		if (!handle)
			return;
		if (!m_fences.empty() && vkWaitForFences(handle, static_cast<std::uint32_t>(m_fences.size()),
			m_fences.data(), VK_TRUE, 5000000000ULL) != VK_SUCCESS)
		{
			std::fputs("FAIL: teardown could not complete owned GPU submissions within 5 seconds; resources remain pending\n", stderr);
			std::_Exit(1);
		}
		for (auto fence : m_fences)
			vkDestroyFence(handle, fence, nullptr);
		for (auto semaphore : m_semaphores)
			vkDestroySemaphore(handle, semaphore, nullptr);
		if (pool)
			vkDestroyCommandPool(handle, pool, nullptr);
		for (auto image : m_images)
			vkDestroyImage(handle, image, nullptr);
		for (auto buffer : m_buffers)
			vkDestroyBuffer(handle, buffer, nullptr);
		for (auto memory : m_allocations)
			vkFreeMemory(handle, memory, nullptr);
		vkDestroyDevice(handle, nullptr);
	}

	void Create(VkPhysicalDevice physicalDevice, std::uint32_t queueFamily, std::uint32_t queueIndex)
	{
		family = queueFamily;
		const std::array<float, 2> priorities{1, 1};
		VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queueInfo.queueFamilyIndex = family;
		queueInfo.queueCount = queueIndex + 1;
		queueInfo.pQueuePriorities = priorities.data();
		const char* extensions[]{VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
		VkDeviceCreateInfo info{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		info.queueCreateInfoCount = 1;
		info.pQueueCreateInfos = &queueInfo;
		info.enabledExtensionCount = 2;
		info.ppEnabledExtensionNames = extensions;
		Check(vkCreateDevice(physicalDevice, &info, nullptr, &handle), "create sharing device");
		vkGetDeviceQueue(handle, family, queueIndex, &queue);
		VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		poolInfo.queueFamilyIndex = family;
		Check(vkCreateCommandPool(handle, &poolInfo, nullptr, &pool), "create command pool");
	}

	template<typename Function>
	Function Get(const char* name)
	{
		auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(handle, name));
		if (!function)
			throw std::runtime_error(std::string("Enabled extension command missing: ") + name);
		return function;
	}

	VkImage CreateImage(bool shared, VkImageUsageFlags usage = ImageUsage)
	{
		VkExternalMemoryImageCreateInfo external{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
		external.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		info.pNext = shared ? &external : nullptr;
		info.imageType = VK_IMAGE_TYPE_2D;
		info.format = ImageFormat;
		info.extent = ImageExtent;
		info.mipLevels = 1;
		info.arrayLayers = 1;
		info.samples = VK_SAMPLE_COUNT_1_BIT;
		info.tiling = VK_IMAGE_TILING_OPTIMAL;
		info.usage = usage;
		VkImage image = VK_NULL_HANDLE;
		Check(vkCreateImage(handle, &info, nullptr, &image), "create private GPU image");
		m_images.push_back(image);
		return image;
	}

	VkDeviceMemory Allocate(VkImage image, std::uint32_t memoryType, VkDeviceSize size, bool shared, int fd = -1)
	{
		FileDescriptor descriptor(fd);
		VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dedicated.image = image;
		VkExportMemoryAllocateInfo exportInfo{VK_STRUCTURE_TYPE_EXPORT_MEMORY_ALLOCATE_INFO};
		exportInfo.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		exportInfo.pNext = &dedicated;
		VkImportMemoryFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR};
		importInfo.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
		importInfo.fd = fd;
		importInfo.pNext = &dedicated;
		VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		info.allocationSize = size;
		info.memoryTypeIndex = memoryType;
		info.pNext = fd >= 0 ? static_cast<void*>(&importInfo) :
			(shared ? static_cast<void*>(&exportInfo) : static_cast<void*>(&dedicated));
		VkDeviceMemory memory = VK_NULL_HANDLE;
		auto result = vkAllocateMemory(handle, &info, nullptr, &memory);
		Check(result, "allocate or import image memory");
		descriptor.Release();
		m_allocations.push_back(memory);
		Check(vkBindImageMemory(handle, image, memory, 0), "bind image memory");
		return memory;
	}

	VkSemaphore CreateSemaphore(bool shared)
	{
		VkExportSemaphoreCreateInfo external{VK_STRUCTURE_TYPE_EXPORT_SEMAPHORE_CREATE_INFO};
		external.handleTypes = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		info.pNext = shared ? &external : nullptr;
		VkSemaphore semaphore = VK_NULL_HANDLE;
		Check(vkCreateSemaphore(handle, &info, nullptr, &semaphore), "create handoff semaphore");
		m_semaphores.push_back(semaphore);
		return semaphore;
	}

	VkCommandBuffer BeginCommands()
	{
		VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		allocation.commandPool = pool;
		allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocation.commandBufferCount = 1;
		VkCommandBuffer commands = VK_NULL_HANDLE;
		Check(vkAllocateCommandBuffers(handle, &allocation, &commands), "allocate command buffer");
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		Check(vkBeginCommandBuffer(commands, &begin), "begin command buffer");
		return commands;
	}

	void Submit(VkCommandBuffer commands, VkSemaphore wait, VkSemaphore signal)
	{
		Check(vkEndCommandBuffer(commands), "end command buffer");
		VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		VkFence fence = VK_NULL_HANDLE;
		Check(vkCreateFence(handle, &fenceInfo, nullptr, &fence), "create completion fence");
		m_fences.push_back(fence);
		VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.waitSemaphoreCount = wait ? 1 : 0;
		submit.pWaitSemaphores = &wait;
		submit.pWaitDstStageMask = &stage;
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &commands;
		submit.signalSemaphoreCount = signal ? 1 : 0;
		submit.pSignalSemaphores = &signal;
		Check(vkQueueSubmit(queue, 1, &submit, fence), "submit GPU handoff");
	}

	void WaitForCompletion()
	{
		auto result = vkWaitForFences(handle, static_cast<std::uint32_t>(m_fences.size()), m_fences.data(),
			VK_TRUE, 5000000000ULL);
		if (result != VK_SUCCESS)
			std::fprintf(stderr, "FAIL: GPU completion returned %d; pending resources cannot be reused\n", result);
		Check(result, "wait for GPU completion (5 seconds)");
	}

	void VerifyImage(VkPhysicalDevice physicalDevice, VkImage image, std::uint32_t frame);

private:
	std::vector<VkImage> m_images;
	std::vector<VkBuffer> m_buffers;
	std::vector<VkDeviceMemory> m_allocations;
	std::vector<VkSemaphore> m_semaphores;
	std::vector<VkFence> m_fences;
};

inline void Barrier(VkCommandBuffer commands, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
	VkAccessFlags sourceAccess, VkAccessFlags targetAccess, std::uint32_t sourceFamily, std::uint32_t targetFamily)
{
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.srcAccessMask = sourceAccess;
	barrier.dstAccessMask = targetAccess;
	barrier.oldLayout = oldLayout;
	barrier.newLayout = newLayout;
	barrier.srcQueueFamilyIndex = sourceFamily;
	barrier.dstQueueFamilyIndex = targetFamily;
	barrier.image = image;
	barrier.subresourceRange = ColorRange;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
		0, 0, nullptr, 0, nullptr, 1, &barrier);
}

inline void Device::VerifyImage(VkPhysicalDevice physicalDevice, VkImage image, std::uint32_t frame)
{
	constexpr VkDeviceSize byteCount = ImageExtent.width * ImageExtent.height * 4;
	VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bufferInfo.size = byteCount;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer buffer = VK_NULL_HANDLE;
	Check(vkCreateBuffer(handle, &bufferInfo, nullptr, &buffer), "create explicit analysis buffer");
	m_buffers.push_back(buffer);
	VkMemoryRequirements requirements{};
	vkGetBufferMemoryRequirements(handle, buffer, &requirements);
	VkPhysicalDeviceMemoryProperties properties{};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
	std::uint32_t memoryType = properties.memoryTypeCount;
	for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		auto flags = properties.memoryTypes[index].propertyFlags;
		if (!(requirements.memoryTypeBits & (1u << index)) || !(flags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			continue;
		memoryType = index;
		if (flags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)
			break;
	}
	if (memoryType == properties.memoryTypeCount)
		throw std::runtime_error("Explicit analysis requires host-visible transfer memory");
	VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = memoryType;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	Check(vkAllocateMemory(handle, &allocation, nullptr, &memory), "allocate explicit analysis buffer");
	m_allocations.push_back(memory);
	Check(vkBindBufferMemory(handle, buffer, memory, 0), "bind explicit analysis buffer");

	auto commands = BeginCommands();
	Barrier(commands, image, VK_IMAGE_LAYOUT_SHADER_READ_ONLY_OPTIMAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
		VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = ImageExtent;
	vkCmdCopyImageToBuffer(commands, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, buffer, 1, &copy);
	VkBufferMemoryBarrier hostBarrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
	hostBarrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	hostBarrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	hostBarrier.srcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostBarrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	hostBarrier.buffer = buffer;
	hostBarrier.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(commands, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0,
		0, nullptr, 1, &hostBarrier, 0, nullptr);
	Submit(commands, VK_NULL_HANDLE, VK_NULL_HANDLE);
	WaitForCompletion();
	void* data = nullptr;
	Check(vkMapMemory(handle, memory, 0, VK_WHOLE_SIZE, 0, &data), "map explicit analysis buffer");
	struct Mapping
	{
		VkDevice device;
		VkDeviceMemory memory;
		~Mapping() { vkUnmapMemory(device, memory); }
	} mapping{handle, memory};
	if (!(properties.memoryTypes[memoryType].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
	{
		VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
		range.memory = memory;
		range.size = VK_WHOLE_SIZE;
		Check(vkInvalidateMappedMemoryRanges(handle, 1, &range), "invalidate explicit analysis buffer");
	}
	std::array<std::uint8_t, 4> expected{0, 0, 0, 255};
	expected[frame] = 255;
	auto pixels = static_cast<const std::uint8_t*>(data);
	for (VkDeviceSize offset = 0; offset < byteCount; ++offset)
	{
		if (pixels[offset] != expected[offset % 4])
			throw std::runtime_error("Retained frame " + std::to_string(frame + 1) +
				" pixel mismatch after source reuse at byte " + std::to_string(offset));
	}
	std::printf("analysis=explicit-readback retainedFrame=%u pixels=4096 exact=passed afterSourceReuse=3\n", frame + 1);
}

inline void ImportSemaphore(Device& from, VkSemaphore source, Device& to, VkSemaphore destination)
{
	VkSemaphoreGetFdInfoKHR exportInfo{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
	exportInfo.semaphore = source;
	exportInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
	int fd = -1;
	Check(from.Get<PFN_vkGetSemaphoreFdKHR>("vkGetSemaphoreFdKHR")(from.handle, &exportInfo, &fd),
		"export semaphore FD");
	FileDescriptor descriptor(fd);
	VkImportSemaphoreFdInfoKHR importInfo{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
	importInfo.semaphore = destination;
	importInfo.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
	importInfo.fd = fd;
	auto result = to.Get<PFN_vkImportSemaphoreFdKHR>("vkImportSemaphoreFdKHR")(to.handle, &importInfo);
	Check(result, "import semaphore FD");
	descriptor.Release();
}

inline bool SupportsSharing(VkPhysicalDevice device, std::uint32_t& family, std::uint32_t& consumerQueue, VkImageUsageFlags usage = ImageUsage)
{
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(device, &properties);
	std::printf("adapter=%s vendor=%04x device=%04x\n", properties.deviceName, properties.vendorID, properties.deviceID);
	if (properties.apiVersion < VK_API_VERSION_1_1)
	{
		std::puts("UNAVAILABLE: Vulkan 1.1 is required by this proof");
		return false;
	}
	VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	properties2.pNext = &identity;
	vkGetPhysicalDeviceProperties2(device, &properties2);
	std::printf("deviceUUID=");
	for (auto value : identity.deviceUUID)
		std::printf("%02x", value);
	std::puts("");
	std::uint32_t count = 0;
	Check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, nullptr), "enumerate device extensions");
	std::vector<VkExtensionProperties> extensions(count);
	Check(vkEnumerateDeviceExtensionProperties(device, nullptr, &count, extensions.data()), "enumerate device extensions");
	for (auto name : {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME})
	{
		if (std::none_of(extensions.begin(), extensions.end(), [name](const auto& extension)
		{
			return std::strcmp(extension.extensionName, name) == 0;
		}))
		{
			std::printf("UNAVAILABLE: %s is absent\n", name);
			return false;
		}
	}
	VkPhysicalDeviceExternalImageFormatInfo externalImage{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_IMAGE_FORMAT_INFO};
	externalImage.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkPhysicalDeviceImageFormatInfo2 format{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_IMAGE_FORMAT_INFO_2};
	format.pNext = &externalImage;
	format.format = ImageFormat;
	format.type = VK_IMAGE_TYPE_2D;
	format.tiling = VK_IMAGE_TILING_OPTIMAL;
	format.usage = usage;
	VkExternalImageFormatProperties externalProperties{VK_STRUCTURE_TYPE_EXTERNAL_IMAGE_FORMAT_PROPERTIES};
	VkImageFormatProperties2 imageProperties{VK_STRUCTURE_TYPE_IMAGE_FORMAT_PROPERTIES_2};
	imageProperties.pNext = &externalProperties;
	auto result = vkGetPhysicalDeviceImageFormatProperties2(device, &format, &imageProperties);
	const auto requiredMemory = VK_EXTERNAL_MEMORY_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_MEMORY_FEATURE_IMPORTABLE_BIT;
	if (result == VK_ERROR_FORMAT_NOT_SUPPORTED || (result == VK_SUCCESS &&
		(externalProperties.externalMemoryProperties.externalMemoryFeatures & requiredMemory) != requiredMemory))
	{
		std::puts("UNAVAILABLE: RGBA8 optimal sampled/transfer image cannot import/export OPAQUE_FD memory");
		return false;
	}
	Check(result, "query external image format");
	VkPhysicalDeviceExternalSemaphoreInfo semaphore{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_SEMAPHORE_INFO};
	semaphore.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkExternalSemaphoreProperties semaphoreProperties{VK_STRUCTURE_TYPE_EXTERNAL_SEMAPHORE_PROPERTIES};
	vkGetPhysicalDeviceExternalSemaphoreProperties(device, &semaphore, &semaphoreProperties);
	const auto requiredSemaphore = VK_EXTERNAL_SEMAPHORE_FEATURE_EXPORTABLE_BIT | VK_EXTERNAL_SEMAPHORE_FEATURE_IMPORTABLE_BIT;
	if ((semaphoreProperties.externalSemaphoreFeatures & requiredSemaphore) != requiredSemaphore)
	{
		std::puts("UNAVAILABLE: binary OPAQUE_FD semaphores cannot be imported/exported");
		return false;
	}
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, nullptr);
	std::vector<VkQueueFamilyProperties> queues(count);
	vkGetPhysicalDeviceQueueFamilyProperties(device, &count, queues.data());
	for (std::uint32_t index = 0; index < count; ++index)
	{
		if (queues[index].queueCount && (queues[index].queueFlags & VK_QUEUE_GRAPHICS_BIT))
		{
			family = index;
			consumerQueue = queues[index].queueCount > 1 ? 1 : 0;
			std::printf("queueFamily=%u producerQueue=0 consumerQueue=%u\n", family, consumerQueue);
			return true;
		}
	}
	std::puts("UNAVAILABLE: no graphics queue");
	return false;
}

inline VkPhysicalDeviceIDProperties Identity(VkPhysicalDevice physicalDevice)
{
	VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
	properties.pNext = &identity;
	vkGetPhysicalDeviceProperties2(physicalDevice, &properties);
	return identity;
}

inline int ExportSemaphore(Device& device, VkSemaphore semaphore)
{
	VkSemaphoreGetFdInfoKHR info{VK_STRUCTURE_TYPE_SEMAPHORE_GET_FD_INFO_KHR};
	info.semaphore = semaphore;
	info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
	int descriptor = -1;
	Check(device.Get<PFN_vkGetSemaphoreFdKHR>("vkGetSemaphoreFdKHR")(device.handle, &info, &descriptor), "export process semaphore");
	return descriptor;
}

inline VkSemaphore ImportSemaphore(Device& device, int descriptor)
{
	FileDescriptor retained(descriptor);
	auto semaphore = device.CreateSemaphore(false);
	VkImportSemaphoreFdInfoKHR info{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
	info.semaphore = semaphore;
	info.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
	info.fd = descriptor;
	Check(device.Get<PFN_vkImportSemaphoreFdKHR>("vkImportSemaphoreFdKHR")(device.handle, &info), "import process semaphore");
	retained.Release();
	return semaphore;
}

}
