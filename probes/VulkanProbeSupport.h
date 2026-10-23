#pragma once

#include "VulkanProbeInstance.h"

#include <cstdint>
#include <stdexcept>
#include <string>
#include <vector>

namespace simxr::probe
{
constexpr VkFormat ImageFormat = VK_FORMAT_R8G8B8A8_UNORM;
constexpr VkExtent3D ImageExtent{64, 64, 1};
constexpr VkImageUsageFlags ImageUsage =
	VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT;
constexpr VkImageSubresourceRange ColorRange{VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};

class FileDescriptor
{
  public:
	explicit FileDescriptor(int value) : m_value(value) {}
	~FileDescriptor();
	FileDescriptor(const FileDescriptor&) = delete;
	FileDescriptor& operator=(const FileDescriptor&) = delete;
	void Release()
	{
		m_value = -1;
	}

  private:
	int m_value;
};

class Device
{
  public:
	Device() = default;
	Device(const Device&) = delete;
	Device& operator=(const Device&) = delete;
	~Device();

	void Create(VkPhysicalDevice physicalDevice, std::uint32_t queueFamily, std::uint32_t queueIndex);
	template <typename Function> Function Get(const char* name)
	{
		auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(handle, name));
		if (!function)
			throw std::runtime_error(std::string("Enabled extension command missing: ") + name);
		return function;
	}
	VkImage CreateImage(bool shared, VkImageUsageFlags usage = ImageUsage);
	VkDeviceMemory Allocate(VkImage image, std::uint32_t memoryType, VkDeviceSize size, bool shared, int fd = -1);
	VkSemaphore CreateSemaphore(bool shared);
	VkCommandBuffer BeginCommands();
	void Submit(VkCommandBuffer commands, VkSemaphore wait, VkSemaphore signal);
	void WaitForCompletion();
	void VerifyImage(VkPhysicalDevice physicalDevice, VkImage image, std::uint32_t frame);

	VkDevice handle = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	std::uint32_t family = 0;

  private:
	std::vector<VkImage> m_images;
	std::vector<VkBuffer> m_buffers;
	std::vector<VkDeviceMemory> m_allocations;
	std::vector<VkSemaphore> m_semaphores;
	std::vector<VkFence> m_fences;
};

void Barrier(VkCommandBuffer commands, VkImage image, VkImageLayout oldLayout, VkImageLayout newLayout,
			 VkAccessFlags sourceAccess, VkAccessFlags targetAccess, std::uint32_t sourceFamily,
			 std::uint32_t targetFamily);
void ImportSemaphore(Device& from, VkSemaphore source, Device& to, VkSemaphore destination);
bool SupportsSharing(VkPhysicalDevice device, std::uint32_t& family, std::uint32_t& consumerQueue,
					 VkImageUsageFlags usage = ImageUsage);
VkPhysicalDeviceIDProperties Identity(VkPhysicalDevice physicalDevice);
int ExportSemaphore(Device& device, VkSemaphore semaphore);
VkSemaphore ImportSemaphore(Device& device, int descriptor);
} // namespace simxr::probe
