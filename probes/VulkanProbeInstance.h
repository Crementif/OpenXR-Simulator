#pragma once

#include <vulkan/vulkan.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <stdexcept>
#include <string>
#include <vector>

namespace simxr::probe
{
inline std::atomic_uint32_t s_validationErrors = 0;

inline void Check(VkResult result, const char* operation)
{
	if (result != VK_SUCCESS)
		throw std::runtime_error(std::string(operation) + " returned " + std::to_string(result));
}

inline VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
	VkDebugUtilsMessageTypeFlagsEXT, const VkDebugUtilsMessengerCallbackDataEXT* message, void*)
{
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
	{
		++s_validationErrors;
		std::fprintf(stderr, "VALIDATION: %s\n", message->pMessage);
	}
	return VK_FALSE;
}

struct Instance
{
	Instance() = default;
	Instance(const Instance&) = delete;
	Instance& operator=(const Instance&) = delete;

	VkInstance handle = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;

	~Instance()
	{
		if (messenger)
		{
			auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
				vkGetInstanceProcAddr(handle, "vkDestroyDebugUtilsMessengerEXT"));
			destroy(handle, messenger, nullptr);
		}
		if (handle)
			vkDestroyInstance(handle, nullptr);
	}

	void Create()
	{
		std::uint32_t count = 0;
		Check(vkEnumerateInstanceLayerProperties(&count, nullptr), "enumerate instance layers");
		std::vector<VkLayerProperties> layers(count);
		Check(vkEnumerateInstanceLayerProperties(&count, layers.data()), "enumerate instance layers");
		const bool validation = std::any_of(layers.begin(), layers.end(), [](const auto& layer)
		{
			return std::strcmp(layer.layerName, "VK_LAYER_KHRONOS_validation") == 0;
		});
		Check(vkEnumerateInstanceExtensionProperties(nullptr, &count, nullptr), "enumerate instance extensions");
		std::vector<VkExtensionProperties> extensions(count);
		Check(vkEnumerateInstanceExtensionProperties(nullptr, &count, extensions.data()), "enumerate instance extensions");
		const bool debugUtils = std::any_of(extensions.begin(), extensions.end(), [](const auto& extension)
		{
			return std::strcmp(extension.extensionName, VK_EXT_DEBUG_UTILS_EXTENSION_NAME) == 0;
		});
		const char* layerName = "VK_LAYER_KHRONOS_validation";
		const char* extensionName = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
		VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		application.pApplicationName = "SimXR Vulkan sharing proof";
		application.apiVersion = VK_API_VERSION_1_1;
		VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
		debug.messageSeverity = VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
		debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
		debug.pfnUserCallback = Validation;
		VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		info.pApplicationInfo = &application;
		info.enabledLayerCount = validation ? 1 : 0;
		info.ppEnabledLayerNames = &layerName;
		info.enabledExtensionCount = debugUtils ? 1 : 0;
		info.ppEnabledExtensionNames = &extensionName;
		info.pNext = debugUtils ? &debug : nullptr;
		Check(vkCreateInstance(&info, nullptr, &handle), "create Vulkan 1.1 instance");
		if (debugUtils)
		{
			auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
				vkGetInstanceProcAddr(handle, "vkCreateDebugUtilsMessengerEXT"));
			Check(create(handle, &debug, nullptr, &messenger), "create debug messenger");
		}
		std::printf("validation=%s\n", validation ? "enabled" : "unavailable");
	}
};

inline std::uint32_t MemoryType(VkPhysicalDevice physicalDevice, std::uint32_t bits)
{
	VkPhysicalDeviceMemoryProperties properties{};
	vkGetPhysicalDeviceMemoryProperties(physicalDevice, &properties);
	for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
	{
		if ((bits & (1u << index)) &&
			(properties.memoryTypes[index].propertyFlags & VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT))
			return index;
	}
	throw std::runtime_error("No common device-local image memory type");
}

}
