#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <vector>
#include <vulkan/vulkan.h>

namespace simxr::headset
{
class OpenXrRuntime;
void CheckVk(VkResult result, const char* operation);

class HeadsetGraphics
{
public:
	HeadsetGraphics() = default;
	~HeadsetGraphics();
	HeadsetGraphics(const HeadsetGraphics&) = delete;
	HeadsetGraphics& operator=(const HeadsetGraphics&) = delete;
	// The runtime outlives this device and any session using it.
	void Open(OpenXrRuntime& runtime, bool validation = false);
	bool Close();
	std::uint32_t ValidationErrors() const;
	bool Drain();
	void PreserveNativeResources();
	bool ResourcesPreserved() const;
	VkInstance Instance() const;
	VkPhysicalDevice PhysicalDevice() const;
	VkDevice Device() const;
	std::uint32_t QueueFamily() const;
	std::uint32_t ApiVersion() const;
	const VkPhysicalDeviceProperties& Properties() const;
	const VkPhysicalDeviceIDProperties& Identity() const;
	const std::vector<const char*>& Extensions() const;

private:
	struct ValidationState
	{
		std::atomic_uint32_t errors{0};
	};

	static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
													 VkDebugUtilsMessageTypeFlagsEXT types,
													 const VkDebugUtilsMessengerCallbackDataEXT* message, void* owner);
	VkInstance m_instance = VK_NULL_HANDLE;
	OpenXrRuntime* m_runtime = nullptr;
	VkDebugUtilsMessengerEXT m_messenger = VK_NULL_HANDLE;
	std::unique_ptr<ValidationState> m_validation = std::make_unique<ValidationState>();
	VkPhysicalDevice m_physicalDevice = VK_NULL_HANDLE;
	VkDevice m_device = VK_NULL_HANDLE;
	std::uint32_t m_queueFamily = 0;
	std::uint32_t m_apiVersion = 0;
	VkPhysicalDeviceProperties m_properties{};
	VkPhysicalDeviceIDProperties m_identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	std::vector<const char*> m_extensions;
	bool m_preserved = false;
};
} // namespace simxr::headset
