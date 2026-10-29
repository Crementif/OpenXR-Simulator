#include "VulkanCreationChecks.h"
#include "Pch.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace simxr::probe
{
static void Require(bool value, const char* message)
{
	if (!value)
		throw std::runtime_error(message);
}

template<class Function>
static Function Command(XrInstance instance, const char* name)
{
	PFN_xrVoidFunction function = nullptr;
	Require(xrGetInstanceProcAddr(instance, name, &function) == XR_SUCCESS && function,
			"Resolve Vulkan creation command");
	return reinterpret_cast<Function>(function);
}

class CreationChecks final
{
public:
	CreationChecks(XrInstance instance, XrSystemId system, VkInstance graphicsInstance, VkPhysicalDevice physicalDevice)
		: m_instance(instance), m_system(system), m_graphicsInstance(graphicsInstance), m_physicalDevice(physicalDevice)
	{
		Require(!s_current, "Creation callback fixture is not nested");
		s_current = this;
	}

	~CreationChecks()
	{
		if (m_secondaryInstance)
			vkDestroyInstance(m_secondaryInstance, nullptr);
		s_current = nullptr;
	}

	CreationChecks(const CreationChecks&) = delete;
	CreationChecks& operator=(const CreationChecks&) = delete;

	void Run()
	{
		const auto memoryProperties = reinterpret_cast<PFN_vkGetPhysicalDeviceMemoryProperties2KHR>(
			vkGetInstanceProcAddr(m_graphicsInstance, "vkGetPhysicalDeviceMemoryProperties2KHR"));
		Require(memoryProperties != nullptr, "Resolve the properties2 alias used by BetterVR");
		VkPhysicalDeviceMemoryProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_MEMORY_PROPERTIES_2};
		memoryProperties(m_physicalDevice, &properties);
		Require(properties.memoryProperties.memoryTypeCount != 0, "Read device memory through the properties2 alias");
		CheckInstance();
		CheckDevice();
		CheckSessionAdmission();
	}

private:
	static VKAPI_ATTR void* VKAPI_CALL Allocate(void*, std::size_t, std::size_t, VkSystemAllocationScope)
	{
		return nullptr;
	}

	static VKAPI_ATTR void* VKAPI_CALL Reallocate(void*, void*, std::size_t, std::size_t, VkSystemAllocationScope)
	{
		return nullptr;
	}

	static VKAPI_ATTR void VKAPI_CALL Free(void*, void*) {}

	static VKAPI_ATTR PFN_vkVoidFunction VKAPI_CALL Resolve(VkInstance instance, const char* name)
	{
		++s_current->m_resolutions;
		s_current->m_resolvedInstance = instance;
		if (s_current->m_missingFunction)
			return nullptr;
		if (std::strcmp(name, "vkCreateInstance") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(CreateInstance);
		if (std::strcmp(name, "vkEnumerateInstanceExtensionProperties") == 0)
			return reinterpret_cast<PFN_vkVoidFunction>(EnumerateInstanceExtensions);
		if (std::strcmp(name, "vkCreateDevice") == 0)
		{
			if (s_current->m_reenter)
				s_current->SelectSecondaryDevice();
			return reinterpret_cast<PFN_vkVoidFunction>(CreateDevice);
		}
		return nullptr;
	}

	static VKAPI_ATTR VkResult VKAPI_CALL EnumerateInstanceExtensions(const char*, std::uint32_t* count,
																	  VkExtensionProperties* properties)
	{
		if (properties && *count)
			std::strcpy(properties[0].extensionName, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME);
		*count = 1;
		return VK_SUCCESS;
	}

	static VKAPI_ATTR VkResult VKAPI_CALL CreateInstance(const VkInstanceCreateInfo* info,
														 const VkAllocationCallbacks* allocator, VkInstance* output)
	{
		++s_current->m_creations;
		Require(info->pApplicationInfo == s_current->m_instanceInfo->pApplicationInfo &&
					info->pNext == s_current->m_instanceInfo->pNext && allocator == &s_current->m_allocator,
				"Preserve instance application, chain and allocator pointers");
		const std::span names(info->ppEnabledExtensionNames, info->enabledExtensionCount);
		Require(std::count_if(names.begin(), names.end(),
							  [](const char* name) {
								  return std::strcmp(name, VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME) == 0;
							  }) == 1,
				"Enable supported properties2 aliases without duplicating application extensions");
		*output = reinterpret_cast<VkInstance>(s_current);
		return s_current->m_nativeResult;
	}

	static VKAPI_ATTR VkResult VKAPI_CALL CreateDevice(VkPhysicalDevice physical, const VkDeviceCreateInfo* info,
													   const VkAllocationCallbacks* allocator, VkDevice* output)
	{
		++s_current->m_creations;
		Require(physical == s_current->m_physicalDevice && allocator == &s_current->m_allocator,
				"Forward selected physical device and exact allocator");
		Require(info->pNext == s_current->m_deviceInfo->pNext &&
					info->pQueueCreateInfos == s_current->m_deviceInfo->pQueueCreateInfos &&
					info->queueCreateInfoCount == s_current->m_deviceInfo->queueCreateInfoCount &&
					info->pEnabledFeatures == s_current->m_deviceInfo->pEnabledFeatures,
				"Preserve native feature chain, features and queue configuration");
		const std::span names(info->ppEnabledExtensionNames, info->enabledExtensionCount);
		Require(std::count_if(names.begin(), names.end(),
							  [](const char* name) {
								  return std::strcmp(name, VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME) == 0;
							  }) == 1,
				"Aggregate required extensions without duplicate application entries");
		*output = reinterpret_cast<VkDevice>(s_current);
		return s_current->m_nativeResult;
	}

	void CheckInstance()
	{
		const auto create = Command<PFN_xrCreateVulkanInstanceKHR>(m_instance, "xrCreateVulkanInstanceKHR");
		VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		application.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo native{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		native.pApplicationInfo = &application;
		m_instanceInfo = &native;
		XrVulkanInstanceCreateInfoKHR request{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
		request.systemId = m_system;
		request.pfnGetInstanceProcAddr = Resolve;
		request.vulkanCreateInfo = &native;
		request.vulkanAllocator = &m_allocator;
		VkInstance untouched = VK_NULL_HANDLE;
		VkResult result = VK_NOT_READY;
		m_missingFunction = true;
		Require(create(m_instance, &request, &untouched, &result) == XR_ERROR_RUNTIME_FAILURE && !untouched &&
					result == VK_NOT_READY && !m_creations,
				"Missing instance resolver leaves native outputs untouched");
		m_missingFunction = false;
		m_nativeResult = VK_ERROR_FEATURE_NOT_PRESENT;
		Require(create(m_instance, &request, &untouched, &result) == XR_SUCCESS && !untouched &&
					result == m_nativeResult && m_resolvedInstance == VK_NULL_HANDLE,
				"Forward instance failure result without publishing a failed native handle");
		m_nativeResult = VK_SUCCESS;
		Require(create(m_instance, &request, &untouched, &result) == XR_SUCCESS &&
					untouched == reinterpret_cast<VkInstance>(this) && result == VK_SUCCESS,
				"Publish the callback's successful instance handle");
		Require(native.pApplicationInfo == &application && !native.enabledExtensionCount &&
					!native.ppEnabledExtensionNames,
				"Application instance configuration remains unchanged");
		const char* extension = VK_KHR_GET_PHYSICAL_DEVICE_PROPERTIES_2_EXTENSION_NAME;
		native.enabledExtensionCount = 1;
		native.ppEnabledExtensionNames = &extension;
		Require(create(m_instance, &request, &untouched, &result) == XR_SUCCESS && result == VK_SUCCESS,
				"Accept an application that already enables properties2");
		m_creations = 0;
		m_resolutions = 0;
	}

	void CheckDevice()
	{
		const auto create = Command<PFN_xrCreateVulkanDeviceKHR>(m_instance, "xrCreateVulkanDeviceKHR");
		VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2};
		const char* extension = VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME;
		const float priority = 1;
		VkDeviceQueueCreateInfo queue{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queue.queueCount = 1;
		queue.pQueuePriorities = &priority;
		VkDeviceCreateInfo native{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		native.pNext = &features;
		native.queueCreateInfoCount = 1;
		native.pQueueCreateInfos = &queue;
		native.enabledExtensionCount = 1;
		native.ppEnabledExtensionNames = &extension;
		m_deviceInfo = &native;
		XrVulkanDeviceCreateInfoKHR request{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
		request.systemId = m_system;
		request.pfnGetInstanceProcAddr = Resolve;
		request.vulkanPhysicalDevice = VK_NULL_HANDLE;
		request.vulkanCreateInfo = &native;
		request.vulkanAllocator = &m_allocator;
		VkDevice untouched = VK_NULL_HANDLE;
		VkResult result = VK_NOT_READY;
		Require(create(m_instance, &request, &untouched, &result) == XR_ERROR_HANDLE_INVALID && !untouched &&
					result == VK_NOT_READY && !m_resolutions && !m_creations,
				"Reject unselected physical device before dispatch or native creation");
		request.vulkanPhysicalDevice = m_physicalDevice;
		m_missingFunction = true;
		Require(create(m_instance, &request, &untouched, &result) == XR_ERROR_RUNTIME_FAILURE && !untouched &&
					result == VK_NOT_READY && !m_creations,
				"Missing device resolver cannot fall back to the global Vulkan loader");
		m_missingFunction = false;
		m_reenter = true;
		m_nativeResult = VK_ERROR_FEATURE_NOT_PRESENT;
		Require(create(m_instance, &request, &untouched, &result) == XR_SUCCESS && !untouched &&
					result == m_nativeResult && m_resolvedInstance == m_graphicsInstance,
				"Resolve device creation against the selected physical device's actual instance");
		m_nativeResult = VK_SUCCESS;
		Require(create(m_instance, &request, &untouched, &result) == XR_SUCCESS &&
					untouched == reinterpret_cast<VkDevice>(this) && result == VK_SUCCESS,
				"Publish the callback's successful device handle");
		Require(native.pNext == &features && native.enabledExtensionCount == 1 &&
					native.ppEnabledExtensionNames == &extension && native.pQueueCreateInfos == &queue,
				"Application device configuration remains unchanged after aggregation");
		Require(!m_reenter && m_secondaryInstance,
				"Resolver reentry can add another physical-device selection without invalidating dispatch");
	}

	void CheckSessionAdmission()
	{
		XrGraphicsBindingVulkanKHR binding{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
		binding.instance = m_graphicsInstance;
		binding.physicalDevice = m_physicalDevice;
		XrSessionCreateInfo creation{XR_TYPE_SESSION_CREATE_INFO};
		creation.systemId = m_system;
		creation.next = &binding;
		XrSession untouched = XR_NULL_HANDLE;
		Require(xrCreateSession(m_instance, &creation, &untouched) == XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING &&
					untouched == XR_NULL_HANDLE,
				"Require the graphics requirements query at session admission");
	}

	void SelectSecondaryDevice()
	{
		m_reenter = false;
		VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		application.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo native{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		native.pApplicationInfo = &application;
		Require(vkCreateInstance(&native, nullptr, &m_secondaryInstance) == VK_SUCCESS,
				"Create secondary resolver-reentry fixture instance");
		const auto select = Command<PFN_xrGetVulkanGraphicsDevice2KHR>(m_instance, "xrGetVulkanGraphicsDevice2KHR");
		XrVulkanGraphicsDeviceGetInfoKHR request{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
		request.systemId = m_system;
		request.vulkanInstance = m_secondaryInstance;
		VkPhysicalDevice physical = VK_NULL_HANDLE;
		Require(select(m_instance, &request, &physical) == XR_SUCCESS && physical,
				"Select secondary physical device during resolver callback");
	}

	inline static thread_local CreationChecks* s_current = nullptr;
	const XrInstance m_instance;
	const XrSystemId m_system;
	const VkInstance m_graphicsInstance;
	const VkPhysicalDevice m_physicalDevice;
	VkAllocationCallbacks m_allocator{nullptr, Allocate, Reallocate, Free, nullptr, nullptr};
	const VkInstanceCreateInfo* m_instanceInfo = nullptr;
	const VkDeviceCreateInfo* m_deviceInfo = nullptr;
	VkInstance m_resolvedInstance = VK_NULL_HANDLE;
	VkInstance m_secondaryInstance = VK_NULL_HANDLE;
	VkResult m_nativeResult = VK_SUCCESS;
	std::uint32_t m_resolutions = 0;
	std::uint32_t m_creations = 0;
	bool m_missingFunction = false;
	bool m_reenter = false;
};

bool VerifyVulkanCreation(XrInstance instance, XrSystemId system, VkInstance graphicsInstance,
						  VkPhysicalDevice physicalDevice)
{
	try
	{
		CreationChecks(instance, system, graphicsInstance, physicalDevice).Run();
		return true;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Vulkan creation contract failed: %s\n", error.what());
		return false;
	}
}
} // namespace simxr::probe
