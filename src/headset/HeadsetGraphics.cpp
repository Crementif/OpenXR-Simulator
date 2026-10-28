#include "headset/HeadsetGraphics.h"
#include "Pch.h"
#include "headset/OpenXrRuntime.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <cstring>
#include <iostream>
#include <openxr/openxr_platform.h>
#include <stdexcept>

namespace simxr::headset
{
void CheckVk(VkResult result, const char* operation)
{
	if (result != VK_SUCCESS)
		throw std::runtime_error(std::string(operation) + " failed with Vulkan result " + std::to_string(result));
}

HeadsetGraphics::~HeadsetGraphics()
{
	Close();
	// A preserved instance may still invoke its callback after this owner exits.
	if (m_preserved)
		m_validation.release();
}

bool HeadsetGraphics::Close()
{
	if (!Drain())
		return false;
	if (m_device)
		vkDestroyDevice(std::exchange(m_device, VK_NULL_HANDLE), nullptr);
	if (m_messenger)
	{
		const auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"));
		destroy(m_instance, m_messenger, nullptr);
		m_messenger = VK_NULL_HANDLE;
	}
	if (m_instance)
		vkDestroyInstance(std::exchange(m_instance, VK_NULL_HANDLE), nullptr);
	return true;
}

bool HeadsetGraphics::Drain()
{
	if (m_preserved)
		return false;
	if (m_device && vkDeviceWaitIdle(m_device) != VK_SUCCESS)
	{
		PreserveNativeResources();
		return false;
	}
	return true;
}

void HeadsetGraphics::PreserveNativeResources()
{
	if (!m_preserved)
		std::cerr << "Headset GPU shutdown is uncertain; preserving native owners until host process exit\n";
	m_preserved = true;
	if (m_runtime)
		m_runtime->PreserveNativeResources();
}

bool HeadsetGraphics::ResourcesPreserved() const
{
	return m_preserved;
}

VKAPI_ATTR VkBool32 VKAPI_CALL HeadsetGraphics::Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
														   VkDebugUtilsMessageTypeFlagsEXT,
														   const VkDebugUtilsMessengerCallbackDataEXT* message,
														   void* owner)
{
	auto& state = *static_cast<ValidationState*>(owner);
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		++state.errors;
	std::cerr << "Headset Vulkan validation: " << message->pMessage << '\n';
	return VK_FALSE;
}

std::uint32_t HeadsetGraphics::ValidationErrors() const
{
	return m_validation->errors.load();
}

void HeadsetGraphics::Open(OpenXrRuntime& runtime, bool validation)
{
	if (m_instance || m_preserved)
		throw std::logic_error("Headset graphics are already open");
	m_runtime = &runtime;
	const auto requirements = reinterpret_cast<PFN_xrGetVulkanGraphicsRequirements2KHR>(
		runtime.Procedure("xrGetVulkanGraphicsRequirements2KHR"));
	XrGraphicsRequirementsVulkan2KHR limits{XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN2_KHR};
	CheckXr(requirements(runtime.Instance(), runtime.System(), &limits), "Read physical Vulkan requirements");
	std::uint32_t loaderVersion = VK_API_VERSION_1_0;
	const auto enumerateVersion = reinterpret_cast<PFN_vkEnumerateInstanceVersion>(
		vkGetInstanceProcAddr(VK_NULL_HANDLE, "vkEnumerateInstanceVersion"));
	if (enumerateVersion)
		CheckVk(enumerateVersion(&loaderVersion), "Read Vulkan loader version");
	const auto maximum =
		std::min<XrVersion>(limits.maxApiVersionSupported,
							XR_MAKE_VERSION(VK_VERSION_MAJOR(loaderVersion), VK_VERSION_MINOR(loaderVersion), 0));
	const auto selected =
		std::max(limits.minApiVersionSupported, std::min<XrVersion>(maximum, XR_MAKE_VERSION(1, 2, 0)));
	if (selected > maximum || selected < XR_MAKE_VERSION(1, 1, 0))
		throw std::runtime_error("Headset sharing requires Vulkan 1.1 or newer within the physical runtime's limits");
	m_apiVersion = VK_MAKE_VERSION(XR_VERSION_MAJOR(selected), XR_VERSION_MINOR(selected), 0);
	VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	application.pApplicationName = "SimXR Headset Host";
	application.apiVersion = m_apiVersion;
	VkInstanceCreateInfo instanceInfo{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	instanceInfo.pApplicationInfo = &application;
	const char* validationLayer = "VK_LAYER_KHRONOS_validation";
	const char* debugExtensions[] = {VK_EXT_DEBUG_UTILS_EXTENSION_NAME, VK_EXT_VALIDATION_FEATURES_EXTENSION_NAME};
	VkValidationFeatureEnableEXT synchronization = VK_VALIDATION_FEATURE_ENABLE_SYNCHRONIZATION_VALIDATION_EXT;
	VkValidationFeaturesEXT validationFeatures{VK_STRUCTURE_TYPE_VALIDATION_FEATURES_EXT};
	validationFeatures.enabledValidationFeatureCount = 1;
	validationFeatures.pEnabledValidationFeatures = &synchronization;
	VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
	debug.messageSeverity =
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT;
	debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT;
	debug.pfnUserCallback = Validation;
	debug.pUserData = m_validation.get();
	validationFeatures.pNext = &debug;
	if (validation)
	{
		std::uint32_t layerCount = 0;
		CheckVk(vkEnumerateInstanceLayerProperties(&layerCount, nullptr), "Enumerate Vulkan validation layers");
		std::vector<VkLayerProperties> layers(layerCount);
		CheckVk(vkEnumerateInstanceLayerProperties(&layerCount, layers.data()), "Read Vulkan validation layers");
		if (std::none_of(layers.begin(), layers.end(),
						 [&](const auto& layer)
						 {
							 return std::strcmp(layer.layerName, validationLayer) == 0;
						 }))
			throw std::runtime_error("Requested Vulkan validation layer is unavailable");
		instanceInfo.enabledLayerCount = 1;
		instanceInfo.ppEnabledLayerNames = &validationLayer;
		instanceInfo.enabledExtensionCount = 2;
		instanceInfo.ppEnabledExtensionNames = debugExtensions;
		instanceInfo.pNext = &validationFeatures;
	}
	XrVulkanInstanceCreateInfoKHR xrInstanceInfo{XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR};
	xrInstanceInfo.systemId = runtime.System();
	xrInstanceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	xrInstanceInfo.vulkanCreateInfo = &instanceInfo;
	VkResult vkResult = VK_SUCCESS;
	const auto createInstance =
		reinterpret_cast<PFN_xrCreateVulkanInstanceKHR>(runtime.Procedure("xrCreateVulkanInstanceKHR"));
	CheckXr(createInstance(runtime.Instance(), &xrInstanceInfo, &m_instance, &vkResult),
			"Create physical Vulkan instance");
	CheckVk(vkResult, "Create physical Vulkan instance");
	if (validation)
	{
		const auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(m_instance, "vkCreateDebugUtilsMessengerEXT"));
		if (!create || !vkGetInstanceProcAddr(m_instance, "vkDestroyDebugUtilsMessengerEXT"))
			throw std::runtime_error("Vulkan debug messenger functions are unavailable");
		CheckVk(create(m_instance, &debug, nullptr, &m_messenger), "Create headset validation messenger");
	}
	XrVulkanGraphicsDeviceGetInfoKHR deviceGetInfo{XR_TYPE_VULKAN_GRAPHICS_DEVICE_GET_INFO_KHR};
	deviceGetInfo.systemId = runtime.System();
	deviceGetInfo.vulkanInstance = m_instance;
	const auto getDevice =
		reinterpret_cast<PFN_xrGetVulkanGraphicsDevice2KHR>(runtime.Procedure("xrGetVulkanGraphicsDevice2KHR"));
	CheckXr(getDevice(runtime.Instance(), &deviceGetInfo, &m_physicalDevice), "Select physical headset GPU");
	VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &m_identity};
	vkGetPhysicalDeviceProperties2(m_physicalDevice, &properties);
	m_properties = properties.properties;
	if (m_properties.apiVersion < VK_API_VERSION_1_1)
		throw std::runtime_error("The physical headset GPU lacks Vulkan 1.1 sharing support");
	m_apiVersion = std::min(m_apiVersion, m_properties.apiVersion);
	if (XR_MAKE_VERSION(VK_VERSION_MAJOR(m_apiVersion), VK_VERSION_MINOR(m_apiVersion),
						VK_VERSION_PATCH(m_apiVersion)) < limits.minApiVersionSupported)
		throw std::runtime_error("Headset GPU Vulkan version is below the physical runtime minimum");
	std::uint32_t count = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &count, nullptr);
	std::vector<VkQueueFamilyProperties> families(count);
	vkGetPhysicalDeviceQueueFamilyProperties(m_physicalDevice, &count, families.data());
	const auto family = std::find_if(families.begin(), families.end(),
									 [](const auto& entry)
									 {
										 return entry.queueCount && (entry.queueFlags & VK_QUEUE_GRAPHICS_BIT);
									 });
	if (family == families.end())
		throw std::runtime_error("The physical headset GPU has no graphics queue");
	m_queueFamily = static_cast<std::uint32_t>(family - families.begin());
	CheckVk(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, nullptr),
			"Enumerate GPU extensions");
	std::vector<VkExtensionProperties> extensions(count);
	CheckVk(vkEnumerateDeviceExtensionProperties(m_physicalDevice, nullptr, &count, extensions.data()),
			"Read GPU extensions");
#ifdef _WIN32
	m_extensions = {VK_KHR_EXTERNAL_MEMORY_WIN32_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_WIN32_EXTENSION_NAME};
#else
	m_extensions = {VK_KHR_EXTERNAL_MEMORY_FD_EXTENSION_NAME, VK_KHR_EXTERNAL_SEMAPHORE_FD_EXTENSION_NAME};
#endif
	if (m_apiVersion < VK_API_VERSION_1_2)
		m_extensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
	for (const auto* name : m_extensions)
		if (std::none_of(extensions.begin(), extensions.end(),
						 [&](const auto& entry)
						 {
							 return std::strcmp(entry.extensionName, name) == 0;
						 }))
			throw std::runtime_error(std::string("Headset GPU sharing requires ") + name);
	VkPhysicalDeviceTimelineSemaphoreFeatures timeline{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &timeline};
	vkGetPhysicalDeviceFeatures2(m_physicalDevice, &features);
	if (!timeline.timelineSemaphore)
		throw std::runtime_error("Headset GPU sharing requires timeline semaphores");
	const float priority = 1;
	VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queueInfo.queueFamilyIndex = m_queueFamily;
	queueInfo.queueCount = 1;
	queueInfo.pQueuePriorities = &priority;
	VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO, &timeline};
	deviceInfo.queueCreateInfoCount = 1;
	deviceInfo.pQueueCreateInfos = &queueInfo;
	deviceInfo.enabledExtensionCount = static_cast<std::uint32_t>(m_extensions.size());
	deviceInfo.ppEnabledExtensionNames = m_extensions.data();
	XrVulkanDeviceCreateInfoKHR xrDeviceInfo{XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR};
	xrDeviceInfo.systemId = runtime.System();
	xrDeviceInfo.pfnGetInstanceProcAddr = vkGetInstanceProcAddr;
	xrDeviceInfo.vulkanPhysicalDevice = m_physicalDevice;
	xrDeviceInfo.vulkanCreateInfo = &deviceInfo;
	const auto createDevice =
		reinterpret_cast<PFN_xrCreateVulkanDeviceKHR>(runtime.Procedure("xrCreateVulkanDeviceKHR"));
	CheckXr(createDevice(runtime.Instance(), &xrDeviceInfo, &m_device, &vkResult), "Create physical Vulkan device");
	CheckVk(vkResult, "Create physical Vulkan device");
}

VkInstance HeadsetGraphics::Instance() const
{
	return m_instance;
}
VkPhysicalDevice HeadsetGraphics::PhysicalDevice() const
{
	return m_physicalDevice;
}
VkDevice HeadsetGraphics::Device() const
{
	return m_device;
}
std::uint32_t HeadsetGraphics::QueueFamily() const
{
	return m_queueFamily;
}
std::uint32_t HeadsetGraphics::ApiVersion() const
{
	return m_apiVersion;
}
const VkPhysicalDeviceProperties& HeadsetGraphics::Properties() const
{
	return m_properties;
}
const VkPhysicalDeviceIDProperties& HeadsetGraphics::Identity() const
{
	return m_identity;
}
const std::vector<const char*>& HeadsetGraphics::Extensions() const
{
	return m_extensions;
}
} // namespace simxr::headset
