#include "graphics/backends/vulkan/VulkanInspectionDevice.h"
#include "Pch.h"
#include "graphics/backends/vulkan/VulkanInspection.h"

#include <cstdio>
#include <cstring>

namespace simxr
{
static constexpr std::array<const char*, 2> s_externalExtensions{VulkanExternalMemoryExtension,
																 VulkanExternalSemaphoreExtension};

static constexpr std::array<const char*, 3> s_deliveryExtensions{
	VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension, VulkanExternalFenceExtension};

static constexpr std::array<const char*, 3> s_timelineExtensions{
	VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension, VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME};

static VKAPI_ATTR VkBool32 VKAPI_CALL Validation(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
												 VkDebugUtilsMessageTypeFlagsEXT,
												 const VkDebugUtilsMessengerCallbackDataEXT* message, void* context)
{
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		static_cast<VulkanInspectionValidation*>(context)->errors.fetch_add(1);
	std::fprintf(stderr, "SimXR inspection Vulkan validation%s: %s\n",
				 severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT ? " error" : "", message->pMessage);
	return VK_FALSE;
}

static std::vector<VkPhysicalDevice> InspectionAdapters(VkInstance instance)
{
	for (std::uint32_t attempt = 0; attempt < 4; ++attempt)
	{
		std::uint32_t count = 0;
		CheckVulkan(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Enumerate inspection adapters");
		if (!count)
			return {};
		std::vector<VkPhysicalDevice> devices(count);
		const auto result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
		if (result == VK_INCOMPLETE)
			continue;
		CheckVulkan(result, "Enumerate inspection adapters");
		devices.resize(count);
		return devices;
	}
	throw VulkanError(VK_INCOMPLETE, "Inspection adapter list did not stabilize");
}

static std::vector<VkExtensionProperties> InspectionExtensions(VkPhysicalDevice physical)
{
	for (std::uint32_t attempt = 0; attempt < 4; ++attempt)
	{
		std::uint32_t count = 0;
		CheckVulkan(vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, nullptr),
					"Enumerate owned inspection extensions");
		if (!count)
			return {};
		std::vector<VkExtensionProperties> extensions(count);
		const auto result = vkEnumerateDeviceExtensionProperties(physical, nullptr, &count, extensions.data());
		if (result == VK_INCOMPLETE)
			continue;
		CheckVulkan(result, "Enumerate owned inspection extensions");
		extensions.resize(count);
		return extensions;
	}
	throw VulkanError(VK_INCOMPLETE, "Inspection extension list did not stabilize");
}

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
											   std::shared_ptr<VulkanInspectionValidation> validation,
											   bool requireTimeline)
{
#ifdef _WIN32
	// The consumer handle is exchanged before submission. Use an explicit timeline
	// value: opaque Win32 fence payloads do not reliably preserve that handshake.
	requireTimeline = true;
#endif
	auto owner = std::make_shared<Native>();
	owner->validation = std::move(validation);
	if (owner->validation)
		owner->validation->liveOwnedDevices.fetch_add(1);
	VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	application.pApplicationName = "SimXR owned inspection";
	std::uint32_t loaderVersion = VK_API_VERSION_1_0;
	vkEnumerateInstanceVersion(&loaderVersion);
	application.apiVersion = std::min(loaderVersion, VK_API_VERSION_1_3);
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
	for (const auto physical : InspectionAdapters(owner->instance))
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
		const auto extensions = InspectionExtensions(physical);
		const auto fenceCaps = VulkanExternalDevice::QueryFence(physical, VK_API_VERSION_1_1);
		const bool fences =
			!requireTimeline && fenceCaps.CanImport() && fenceCaps.CanExport() &&
			std::any_of(extensions.begin(), extensions.end(),
						[](const auto& extension)
						{
							return std::strcmp(extension.extensionName, VulkanExternalFenceExtension) == 0;
						});
		const auto timelineCaps = VulkanExternalDevice::QueryTimeline(physical, VK_API_VERSION_1_1);
		const bool timeline = !fences && timelineCaps.CanImportSemaphore() && timelineCaps.CanExportSemaphore();
		if (requireTimeline && !timeline)
			throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Required inspection timeline sharing unavailable");
		std::uint32_t count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, nullptr);
		std::vector<VkQueueFamilyProperties> families(count);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &count, families.data());
		std::uint32_t encodeFamily = VK_QUEUE_FAMILY_IGNORED, decodeFamily = VK_QUEUE_FAMILY_IGNORED;
		const std::array videoExtensions{VK_KHR_VIDEO_QUEUE_EXTENSION_NAME, VK_KHR_VIDEO_ENCODE_QUEUE_EXTENSION_NAME,
										 VK_KHR_VIDEO_ENCODE_H264_EXTENSION_NAME,
										 VK_KHR_VIDEO_DECODE_QUEUE_EXTENSION_NAME,
										 VK_KHR_VIDEO_DECODE_H264_EXTENSION_NAME};
		bool video = application.apiVersion >= VK_API_VERSION_1_3 && properties.apiVersion >= VK_API_VERSION_1_3;
		for (const auto name : videoExtensions)
			video &= std::any_of(extensions.begin(), extensions.end(),
								 [name](const auto& entry)
								 {
									 return std::strcmp(entry.extensionName, name) == 0;
								 });
		if (video)
		{
			std::vector<VkQueueFamilyVideoPropertiesKHR> codecs(count,
																{VK_STRUCTURE_TYPE_QUEUE_FAMILY_VIDEO_PROPERTIES_KHR});
			std::vector<VkQueueFamilyProperties2> queues(count, {VK_STRUCTURE_TYPE_QUEUE_FAMILY_PROPERTIES_2});
			for (std::uint32_t i = 0; i < count; ++i)
				queues[i].pNext = &codecs[i];
			vkGetPhysicalDeviceQueueFamilyProperties2(physical, &count, queues.data());
			for (std::uint32_t i = 0; i < count; ++i)
			{
				if (codecs[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR)
					encodeFamily = i;
				if (codecs[i].videoCodecOperations & VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR)
					decodeFamily = i;
			}
			video = encodeFamily != VK_QUEUE_FAMILY_IGNORED && decodeFamily != VK_QUEUE_FAMILY_IGNORED;
		}
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
			std::vector<VkDeviceQueueCreateInfo> queues{queue};
			if (video)
				for (const auto videoFamily : {encodeFamily, decodeFamily})
					if (std::none_of(queues.begin(), queues.end(),
									 [videoFamily](const auto& entry)
									 {
										 return entry.queueFamilyIndex == videoFamily;
									 }))
					{
						auto videoQueue = queue;
						videoQueue.queueFamilyIndex = videoFamily;
						queues.push_back(videoQueue);
					}
			const auto base = fences	 ? std::span<const char* const>(s_deliveryExtensions)
							  : timeline ? std::span<const char* const>(s_timelineExtensions)
										 : std::span<const char* const>(s_externalExtensions);
			m_extensions.assign(base.begin(), base.end());
			if (video)
				m_extensions.insert(m_extensions.end(), videoExtensions.begin(), videoExtensions.end());
			device.queueCreateInfoCount = static_cast<std::uint32_t>(queues.size());
			device.pQueueCreateInfos = queues.data();
			device.enabledExtensionCount = static_cast<std::uint32_t>(m_extensions.size());
			device.ppEnabledExtensionNames = m_extensions.data();
			VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature{
				VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
			timelineFeature.timelineSemaphore = timeline;
			device.pNext = timeline ? &timelineFeature : nullptr;
			CheckVulkan(vkCreateDevice(physical, &device, nullptr, &owner->device), "Create owned inspection device");
			m_device = std::make_shared<VulkanDevice>(
				VulkanBinding{owner->instance, physical, owner->device, family, 0}, owner);
			if (video)
			{
				m_device->encodeFamily = encodeFamily;
				m_device->decodeFamily = decodeFamily;
			}
			m_consumerFences = fences;
			m_consumerTimeline = timeline;
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
	return m_extensions;
}
bool VulkanInspectionDevice::ConsumerFencesEnabled() const noexcept
{
	return m_consumerFences;
}
bool VulkanInspectionDevice::ConsumerTimelineEnabled() const noexcept
{
	return m_consumerTimeline;
}
} // namespace simxr
