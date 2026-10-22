#include "graphics/backends/vulkan/VulkanInspectionDevice.h"
#include "graphics/backends/vulkan/VulkanInspection.h"

#include <algorithm>
#include <array>
#include <cstdio>
#include <cstring>

namespace simxr
{
namespace
{
constexpr std::array<const char*, 2> ExternalExtensions{VulkanExternalMemoryExtension,
														VulkanExternalSemaphoreExtension};

constexpr std::array<const char*, 3> DeliveryExtensions{VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension,
														VulkanExternalFenceExtension};

VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
										  VkDebugUtilsMessageTypeFlagsEXT,
										  const VkDebugUtilsMessengerCallbackDataEXT* message, void* context)
{
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		static_cast<VulkanInspectionValidation*>(context)->errors.fetch_add(1);
	std::fprintf(stderr, "SimXR inspection Vulkan validation%s: %s\n",
				 severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? " error" : "", message->pMessage);
	return VK_FALSE;
}

} // namespace

struct VulkanInspectionDevice::Native
{
	VkInstance instance = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	std::shared_ptr<VulkanInspectionValidation> validation;
	~Native()
	{
		if (device)
			vkDestroyDevice(device, nullptr);
		if (messenger)
		{
			auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
				vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
			destroy(instance, messenger, nullptr);
		}
		if (instance)
			vkDestroyInstance(instance, nullptr);
		if (validation)
			validation->liveOwnedDevices.fetch_sub(1);
	}
};

VulkanInspectionDevice::VulkanInspectionDevice(const VulkanExternalIdentity& identity,
											   std::shared_ptr<VulkanInspectionValidation> validation)
{
	auto owner = std::make_shared<Native>();
	owner->validation = std::move(validation);
	if (owner->validation)
		owner->validation->liveOwnedDevices.fetch_add(1);
	VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	application.pApplicationName = "SimXR owned inspection";
	application.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo create{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	create.pApplicationInfo = &application;
	const char* layer = "VK_LAYER_KHRONOS_validation";
	const char* debugExtension = VK_EXT_DEBUG_UTILS_EXTENSION_NAME;
	VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
	if (owner->validation)
	{
		debug.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
		debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
							VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
							VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debug.pfnUserCallback = Validation;
		debug.pUserData = owner->validation.get();
		create.pNext = &debug;
		create.enabledLayerCount = 1;
		create.ppEnabledLayerNames = &layer;
		create.enabledExtensionCount = 1;
		create.ppEnabledExtensionNames = &debugExtension;
	}
	CheckVulkan(vkCreateInstance(&create, nullptr, &owner->instance), "Create owned inspection instance");
	if (owner->validation)
	{
		auto make = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(owner->instance, "vkCreateDebugUtilsMessengerEXT"));
		if (!make)
			throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Inspection debug messenger unavailable");
		CheckVulkan(make(owner->instance, &debug, nullptr, &owner->messenger),
					"Create inspection validation messenger");
	}
	std::uint32_t count = 0;
	CheckVulkan(vkEnumeratePhysicalDevices(owner->instance, &count, nullptr), "Enumerate inspection adapters");
	std::vector<VkPhysicalDevice> devices(count);
	CheckVulkan(vkEnumeratePhysicalDevices(owner->instance, &count, devices.data()), "Enumerate inspection adapters");
	for (auto physical : devices)
	{
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(physical, &properties);
		if (properties.apiVersion < VK_API_VERSION_1_1)
			continue;
		VkPhysicalDeviceIDProperties id{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
		VkPhysicalDeviceProperties2 extended{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2};
		extended.pNext = &id;
		vkGetPhysicalDeviceProperties2(physical, &extended);
		if (!std::equal(identity.deviceUuid.begin(), identity.deviceUuid.end(), id.deviceUUID) ||
			!std::equal(identity.driverUuid.begin(), identity.driverUuid.end(), id.driverUUID))
			continue;
		std::uint32_t extensionCount = 0;
		CheckVulkan(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, nullptr),
					"Enumerate owned inspection extensions");
		std::vector<VkExtensionProperties> extensions(extensionCount);
		CheckVulkan(vkEnumerateDeviceExtensionProperties(physical, nullptr, &extensionCount, extensions.data()),
					"Enumerate owned inspection extensions");
		const auto fenceCaps = VulkanExternalDevice::QueryFence(physical, VK_API_VERSION_1_1);
		const bool fences =
			fenceCaps.CanImport() && fenceCaps.CanExport() &&
			std::any_of(extensions.begin(), extensions.end(),
						[](const auto& extension)
						{
							return std::strcmp(extension.extensionName, VulkanExternalFenceExtension) == 0;
						});
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
		for (std::uint32_t family = 0; family < count; ++family)
		{
			if (!families[family].queueCount || !(families[family].queueFlags & VK_QUEUE_GRAPHICS_BIT))
				continue;
			const float priority = 1;
			VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
			queue.queueFamilyIndex = family;
			queue.queueCount = 1;
			queue.pQueuePriorities = &priority;
			VkDeviceCreateInfo device{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
			device.queueCreateInfoCount = 1;
			device.pQueueCreateInfos = &queue;
			device.enabledExtensionCount = fences ? DeliveryExtensions.size() : ExternalExtensions.size();
			device.ppEnabledExtensionNames = fences ? DeliveryExtensions.data() : ExternalExtensions.data();
			CheckVulkan(vkCreateDevice(physical, &device, nullptr, &owner->device), "Create owned inspection device");
			m_device = std::make_shared<VulkanDevice>(
				VulkanBinding{owner->instance, physical, owner->device, family, 0}, owner);
			m_consumerFences = fences;
			return;
		}
	}
	throw VulkanError(VK_ERROR_INCOMPATIBLE_DRIVER, "No matching inspection device and driver identity");
}

const std::shared_ptr<VulkanDevice>& VulkanInspectionDevice::Device() const noexcept
{
	return m_device;
}
std::span<const char* const> VulkanInspectionDevice::EnabledExtensions() const noexcept
{
	if (m_consumerFences)
		return DeliveryExtensions;
	return ExternalExtensions;
}
bool VulkanInspectionDevice::ConsumerFencesEnabled() const noexcept
{
	return m_consumerFences;
}
} // namespace simxr
