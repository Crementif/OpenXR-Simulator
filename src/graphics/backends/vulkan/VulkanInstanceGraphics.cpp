#include "graphics/backends/vulkan/VulkanInstanceGraphics.h"
#include "Pch.h"
#include "graphics/backends/vulkan/VulkanSwapchain.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

#include "graphics/backends/vulkan/VulkanExternal.h"
#include "runtime/validation/StructureChain.h"

#include <cstring>

namespace simxr
{
static XrResult WriteExtensions(std::string_view names, std::uint32_t capacity, std::uint32_t* count, char* output)
{
	if (!count || (capacity && !output))
		return XR_ERROR_VALIDATION_FAILURE;
	*count = static_cast<std::uint32_t>(names.size() + 1);
	if (!capacity)
		return XR_SUCCESS;
	if (capacity < *count)
		return XR_ERROR_SIZE_INSUFFICIENT;
	std::memcpy(output, names.data(), names.size());
	output[names.size()] = '\0';
	return XR_SUCCESS;
}

static VkPhysicalDevice SelectPhysicalDevice(VkInstance instance)
{
	for (std::uint32_t attempt = 0; attempt < 4; ++attempt)
	{
		std::uint32_t count = 0;
		CheckVulkan(vkEnumeratePhysicalDevices(instance, &count, nullptr), "Enumerate runtime Vulkan devices");
		if (!count)
			return VK_NULL_HANDLE;
		std::vector<VkPhysicalDevice> devices(count);
		const auto result = vkEnumeratePhysicalDevices(instance, &count, devices.data());
		if (result == VK_INCOMPLETE)
			continue;
		CheckVulkan(result, "Enumerate runtime Vulkan devices");
		devices.resize(count);
		if (devices.empty())
			return VK_NULL_HANDLE;
		for (const auto device : devices)
		{
			VkPhysicalDeviceProperties properties{};
			vkGetPhysicalDeviceProperties(device, &properties);
			if (properties.deviceType == VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
				return device;
		}
		return devices.front();
	}
	throw VulkanError(VK_INCOMPLETE, "Vulkan physical device list did not stabilize");
}

class VulkanProbeInstance final
{
  public:
	VulkanProbeInstance()
	{
		VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		application.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo creation{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		creation.pApplicationInfo = &application;
		CheckVulkan(vkCreateInstance(&creation, nullptr, &m_instance), "Create Vulkan capability query instance");
	}
	~VulkanProbeInstance()
	{
		vkDestroyInstance(m_instance, nullptr);
	}
	VulkanProbeInstance(const VulkanProbeInstance&) = delete;
	VulkanProbeInstance& operator=(const VulkanProbeInstance&) = delete;
	VkPhysicalDevice PhysicalDevice() const
	{
		const auto selected = SelectPhysicalDevice(m_instance);
		if (!selected)
			throw VulkanError(VK_ERROR_INCOMPATIBLE_DRIVER, "No Vulkan physical device available");
		return selected;
	}

  private:
	VkInstance m_instance = VK_NULL_HANDLE;
};

static bool ValidNames(std::uint32_t count, const char* const* names)
{
	if (!count)
		return true;
	if (!names)
		return false;
	const std::span selected(names, count);
	return std::all_of(selected.begin(), selected.end(),
					   [](const char* name)
					   {
						   return name && *name;
					   });
}

std::vector<const char*> VulkanSharingExtensions(VkPhysicalDevice physicalDevice)
{
	if (!physicalDevice)
		return {};
	VulkanExternalImageSpec image;
	image.extent = {1, 1, 1};
	const auto capabilities = VulkanExternalDevice::Query(physicalDevice, image, VK_API_VERSION_1_1);
	if (!capabilities.CanExportImage() || !capabilities.CanImportImage() || !capabilities.CanExportSemaphore() ||
		!capabilities.CanImportSemaphore())
		return {};
	return {VulkanExternalMemoryExtension, VulkanExternalSemaphoreExtension};
}

XrResult VulkanInstanceGraphics::Requirements(XrSystemId system, XrGraphicsRequirementsVulkanKHR* output)
{
	if (!output || output->type != XR_TYPE_GRAPHICS_REQUIREMENTS_VULKAN_KHR)
		return XR_ERROR_VALIDATION_FAILURE;
	if (system != 1)
		return XR_ERROR_SYSTEM_INVALID;
	if (XR_FAILED(StructureChain(output->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	output->minApiVersionSupported = XR_MAKE_VERSION(1, 1, 0);
	output->maxApiVersionSupported = XR_MAKE_VERSION(1, 4, 0);
	m_requirementsRead = true;
	return XR_SUCCESS;
}

XrResult VulkanInstanceGraphics::MaxSampleCount(std::uint32_t& output)
{
	if (!m_maxSampleCount)
	{
		try
		{
			const VulkanProbeInstance probe;
			const auto physical = probe.PhysicalDevice();
			VkSampleCountFlags common = VK_SAMPLE_COUNT_1_BIT | VK_SAMPLE_COUNT_2_BIT | VK_SAMPLE_COUNT_4_BIT |
										VK_SAMPLE_COUNT_8_BIT | VK_SAMPLE_COUNT_16_BIT | VK_SAMPLE_COUNT_32_BIT |
										VK_SAMPLE_COUNT_64_BIT;
			for (const auto format : VulkanSwapchain::ColorFormats())
			{
				VkImageFormatProperties supported{};
				CheckVulkan(vkGetPhysicalDeviceImageFormatProperties(
								physical, static_cast<VkFormat>(format), VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
								VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
									VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT,
								0, &supported),
							"Query swapchain sample counts");
				common &= supported.sampleCounts;
			}
			std::uint32_t maximum = 1;
			for (std::uint32_t samples = 2; samples <= 64; samples *= 2)
				if (common & samples)
					maximum = samples;
			m_maxSampleCount = maximum;
		}
		catch (const VulkanError&)
		{
			return XR_ERROR_RUNTIME_FAILURE;
		}
	}
	output = *m_maxSampleCount;
	return XR_SUCCESS;
}

XrResult VulkanInstanceGraphics::InstanceExtensions(XrSystemId system, std::uint32_t capacity, std::uint32_t* count,
													char* output) const
{
	if (system != 1)
		return XR_ERROR_SYSTEM_INVALID;
	return WriteExtensions("", capacity, count, output);
}

XrResult VulkanInstanceGraphics::DeviceExtensions(XrSystemId system, std::uint32_t capacity, std::uint32_t* count,
												  char* output)
{
	if (system != 1)
		return XR_ERROR_SYSTEM_INVALID;
	if (!count || (capacity && !output))
		return XR_ERROR_VALIDATION_FAILURE;
	if (!m_deviceExtensions)
	{
		try
		{
			const VulkanProbeInstance probe;
			std::string names;
			for (const auto* name : VulkanSharingExtensions(probe.PhysicalDevice()))
			{
				if (!names.empty())
					names += ' ';
				names += name;
			}
			m_deviceExtensions = std::move(names);
		}
		catch (const VulkanError&)
		{
			return XR_ERROR_RUNTIME_FAILURE;
		}
	}
	return WriteExtensions(*m_deviceExtensions, capacity, count, output);
}

std::optional<VulkanInstanceGraphics::Selection> VulkanInstanceGraphics::FindSelection(VkPhysicalDevice device) const
{
	const auto found = std::find_if(m_selections.begin(), m_selections.end(),
									[device](const Selection& selection)
									{
										return selection.physicalDevice == device;
									});
	if (found == m_selections.end())
		return {};
	return *found;
}

XrResult VulkanInstanceGraphics::PhysicalDevice(XrSystemId system, VkInstance instance, VkPhysicalDevice* output)
{
	if (system != 1)
		return XR_ERROR_SYSTEM_INVALID;
	if (!instance || !output)
		return XR_ERROR_VALIDATION_FAILURE;
	try
	{
		const auto selected = SelectPhysicalDevice(instance);
		if (!selected)
			return XR_ERROR_RUNTIME_FAILURE;
		const auto existing = std::find_if(m_selections.begin(), m_selections.end(),
										   [selected](const Selection& selection)
										   {
											   return selection.physicalDevice == selected;
										   });
		if (existing == m_selections.end())
			m_selections.push_back({selected, instance});
		else
			existing->instance = instance;
		*output = selected;
		return XR_SUCCESS;
	}
	catch (const VulkanError&)
	{
		return XR_ERROR_RUNTIME_FAILURE;
	}
}

XrResult VulkanInstanceGraphics::CreateInstance(const XrVulkanInstanceCreateInfoKHR* info, VkInstance* output,
												VkResult* result) const
{
	if (!info || info->type != XR_TYPE_VULKAN_INSTANCE_CREATE_INFO_KHR || !info->vulkanCreateInfo ||
		!info->pfnGetInstanceProcAddr || !output || !result || info->createFlags ||
		info->vulkanCreateInfo->sType != VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO ||
		!ValidNames(info->vulkanCreateInfo->enabledExtensionCount, info->vulkanCreateInfo->ppEnabledExtensionNames) ||
		!ValidNames(info->vulkanCreateInfo->enabledLayerCount, info->vulkanCreateInfo->ppEnabledLayerNames) ||
		XR_FAILED(StructureChain(info->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	if (info->systemId != 1)
		return XR_ERROR_SYSTEM_INVALID;
	const auto create =
		reinterpret_cast<PFN_vkCreateInstance>(info->pfnGetInstanceProcAddr(VK_NULL_HANDLE, "vkCreateInstance"));
	if (!create)
		return XR_ERROR_RUNTIME_FAILURE;
	VkInstance created = VK_NULL_HANDLE;
	*result = create(info->vulkanCreateInfo, info->vulkanAllocator, &created);
	if (*result == VK_SUCCESS)
		*output = created;
	return XR_SUCCESS;
}

XrResult VulkanInstanceGraphics::CreateDevice(const XrVulkanDeviceCreateInfoKHR* info, VkDevice* output,
											  VkResult* result) const
{
	if (!info || info->type != XR_TYPE_VULKAN_DEVICE_CREATE_INFO_KHR || !info->vulkanCreateInfo ||
		!info->pfnGetInstanceProcAddr || !output || !result || info->createFlags ||
		info->vulkanCreateInfo->sType != VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO ||
		!ValidNames(info->vulkanCreateInfo->enabledExtensionCount, info->vulkanCreateInfo->ppEnabledExtensionNames) ||
		XR_FAILED(StructureChain(info->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	if (info->systemId != 1)
		return XR_ERROR_SYSTEM_INVALID;
	const auto selection = FindSelection(info->vulkanPhysicalDevice);
	if (!selection)
		return XR_ERROR_HANDLE_INVALID;
	const auto create =
		reinterpret_cast<PFN_vkCreateDevice>(info->pfnGetInstanceProcAddr(selection->instance, "vkCreateDevice"));
	if (!create)
		return XR_ERROR_RUNTIME_FAILURE;
	const auto& application = *info->vulkanCreateInfo;
	std::vector<const char*> extensions;
	if (application.enabledExtensionCount)
		extensions.assign(application.ppEnabledExtensionNames,
						  application.ppEnabledExtensionNames + application.enabledExtensionCount);
	try
	{
		for (const auto* required : VulkanSharingExtensions(selection->physicalDevice))
			if (std::none_of(extensions.begin(), extensions.end(),
							 [required](const char* existing)
							 {
								 return std::strcmp(existing, required) == 0;
							 }))
				extensions.push_back(required);
	}
	catch (const VulkanError& error)
	{
		if (error.Result() == VK_ERROR_OUT_OF_HOST_MEMORY || error.Result() == VK_ERROR_OUT_OF_DEVICE_MEMORY)
			return XR_ERROR_OUT_OF_MEMORY;
		return XR_ERROR_RUNTIME_FAILURE;
	}
	auto creation = application;
	creation.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
	creation.ppEnabledExtensionNames = extensions.data();
	VkDevice created = VK_NULL_HANDLE;
	*result = create(selection->physicalDevice, &creation, info->vulkanAllocator, &created);
	if (*result == VK_SUCCESS)
		*output = created;
	return XR_SUCCESS;
}

XrResult VulkanInstanceGraphics::SessionBinding(const XrSessionCreateInfo& info, VulkanBinding& output) const
{
	if (!m_requirementsRead)
		return XR_ERROR_GRAPHICS_REQUIREMENTS_CALL_MISSING;
	const StructureChain chain(info.next);
	const std::array bindings{XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR};
	if (XR_FAILED(chain.Validate(bindings)))
		return XR_ERROR_VALIDATION_FAILURE;
	const auto* binding = chain.Find<XrGraphicsBindingVulkanKHR>(XR_TYPE_GRAPHICS_BINDING_VULKAN_KHR);
	if (!binding || !binding->instance || !binding->physicalDevice || !binding->device)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	const auto selection = FindSelection(binding->physicalDevice);
	if (!selection || selection->instance != binding->instance)
		return XR_ERROR_GRAPHICS_DEVICE_INVALID;
	output = {binding->instance, binding->physicalDevice, binding->device, binding->queueFamilyIndex,
			  binding->queueIndex};
	return XR_SUCCESS;
}
} // namespace simxr
