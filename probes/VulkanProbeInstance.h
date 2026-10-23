#pragma once

#include <atomic>
#include <cstdint>
#include <vulkan/vulkan.h>

namespace simxr::probe
{
extern std::atomic_uint32_t s_validationErrors;

void Check(VkResult result, const char* operation);

struct Instance
{
	Instance() = default;
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;
	~Instance();

	void Create(std::uint32_t apiVersion = VK_API_VERSION_1_1);

	VkInstance handle = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
};

std::uint32_t MemoryType(VkPhysicalDevice physicalDevice, std::uint32_t bits);
} // namespace simxr::probe
