#include "Pch.h"
#include "VulkanProbeInstance.h"

#include <cstdio>
#include <cstring>
#include <stdexcept>

namespace simxr::probe
{
static bool Has(std::span<const VkExtensionProperties> extensions, const char* name)
{
	return std::any_of(extensions.begin(), extensions.end(),
					   [&](const auto& entry) { return std::strcmp(entry.extensionName, name) == 0; });
}

class DensityMap
{
public:
	DensityMap() = default;
	DensityMap(const DensityMap&) = delete;
	DensityMap& operator=(const DensityMap&) = delete;

	~DensityMap()
	{
		if (m_view)
			vkDestroyImageView(m_device, m_view, nullptr);
		if (m_image)
			vkDestroyImage(m_device, m_image, nullptr);
		if (m_memory)
			vkFreeMemory(m_device, m_memory, nullptr);
		if (m_device)
			vkDestroyDevice(m_device, nullptr);
	}

	void Create(VkPhysicalDevice physicalDevice, const VkPhysicalDeviceFragmentDensityMapFeaturesEXT& supported)
	{
		std::uint32_t count = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, nullptr);
		std::vector<VkQueueFamilyProperties> queues(count);
		vkGetPhysicalDeviceQueueFamilyProperties(physicalDevice, &count, queues.data());
		auto queue = std::find_if(queues.begin(), queues.end(), [](const auto& entry) {
			return entry.queueCount && (entry.queueFlags & VK_QUEUE_GRAPHICS_BIT);
		});
		if (queue == queues.end())
			throw std::runtime_error("Density-map device has no graphics queue");
		const float priority = 1;
		VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queueInfo.queueFamilyIndex = static_cast<std::uint32_t>(queue - queues.begin());
		queueInfo.queueCount = 1;
		queueInfo.pQueuePriorities = &priority;
		const char* extension = VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME;
		VkPhysicalDeviceFragmentDensityMapFeaturesEXT enabled{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
		enabled.fragmentDensityMap = VK_TRUE;
		enabled.fragmentDensityMapDynamic = supported.fragmentDensityMapDynamic;
		enabled.fragmentDensityMapNonSubsampledImages = supported.fragmentDensityMapNonSubsampledImages;
		VkDeviceCreateInfo deviceInfo{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		deviceInfo.pNext = &enabled;
		deviceInfo.queueCreateInfoCount = 1;
		deviceInfo.pQueueCreateInfos = &queueInfo;
		deviceInfo.enabledExtensionCount = 1;
		deviceInfo.ppEnabledExtensionNames = &extension;
		Check(vkCreateDevice(physicalDevice, &deviceInfo, nullptr, &m_device),
			  "create device with enabled fragment density-map features");
		VkImageCreateInfo imageInfo{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
		imageInfo.imageType = VK_IMAGE_TYPE_2D;
		imageInfo.format = VK_FORMAT_R8G8_UNORM;
		imageInfo.extent = {32, 32, 1};
		imageInfo.mipLevels = 1;
		imageInfo.arrayLayers = 2;
		imageInfo.samples = VK_SAMPLE_COUNT_1_BIT;
		imageInfo.tiling = VK_IMAGE_TILING_OPTIMAL;
		imageInfo.usage = VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
		Check(vkCreateImage(m_device, &imageInfo, nullptr, &m_image), "create real stereo density-map image");
		VkMemoryRequirements requirements{};
		vkGetImageMemoryRequirements(m_device, m_image, &requirements);
		VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dedicated.image = m_image;
		VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocation.pNext = &dedicated;
		allocation.allocationSize = requirements.size;
		allocation.memoryTypeIndex = MemoryType(physicalDevice, requirements.memoryTypeBits);
		Check(vkAllocateMemory(m_device, &allocation, nullptr, &m_memory), "allocate density-map memory");
		Check(vkBindImageMemory(m_device, m_image, m_memory, 0), "bind density-map memory");
		VkImageViewCreateInfo viewInfo{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		viewInfo.image = m_image;
		viewInfo.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
		viewInfo.format = imageInfo.format;
		viewInfo.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
		viewInfo.flags =
			enabled.fragmentDensityMapDynamic ? VK_IMAGE_VIEW_CREATE_FRAGMENT_DENSITY_MAP_DYNAMIC_BIT_EXT : 0;
		Check(vkCreateImageView(m_device, &viewInfo, nullptr, &m_view), "create density-map image view");
		std::puts("densityMapAllocation=passed format=R8G8_UNORM extent=32x32 layers=2 features=explicitly-enabled "
				  "rendering=not-qualified");
	}

private:
	VkDevice m_device = VK_NULL_HANDLE;
	VkImage m_image = VK_NULL_HANDLE;
	VkImageView m_view = VK_NULL_HANDLE;
	VkDeviceMemory m_memory = VK_NULL_HANDLE;
};

static bool Inspect(VkPhysicalDevice physicalDevice)
{
	VkPhysicalDeviceProperties properties{};
	vkGetPhysicalDeviceProperties(physicalDevice, &properties);
	VkPhysicalDeviceIDProperties identity{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
	VkPhysicalDeviceProperties2 identityQuery{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &identity};
	vkGetPhysicalDeviceProperties2(physicalDevice, &identityQuery);
	std::printf("adapter=%s deviceUUID=", properties.deviceName);
	for (const auto byte : identity.deviceUUID)
		std::printf("%02x", byte);
	std::puts("");
	std::uint32_t count = 0;
	Check(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, nullptr),
		  "enumerate foveation capabilities");
	std::vector<VkExtensionProperties> extensions(count);
	Check(vkEnumerateDeviceExtensionProperties(physicalDevice, nullptr, &count, extensions.data()),
		  "enumerate foveation capabilities");
	std::printf("fragmentShadingRateExtension=%s (distinct from fragment density maps)\n",
				Has(extensions, VK_KHR_FRAGMENT_SHADING_RATE_EXTENSION_NAME) ? "present" : "absent");
	if (!Has(extensions, VK_EXT_FRAGMENT_DENSITY_MAP_EXTENSION_NAME))
	{
		std::puts("fbVulkanDensityMap=unavailable reason=VK_EXT_fragment_density_map-absent; do not advertise "
				  "XR_FB_foveation_vulkan");
		return false;
	}
	VkPhysicalDeviceFragmentDensityMapFeaturesEXT density{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_FEATURES_EXT};
	VkPhysicalDeviceFeatures2 features{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FEATURES_2, &density};
	vkGetPhysicalDeviceFeatures2(physicalDevice, &features);
	VkPhysicalDeviceFragmentDensityMapPropertiesEXT limits{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_FRAGMENT_DENSITY_MAP_PROPERTIES_EXT};
	VkPhysicalDeviceProperties2 properties2{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &limits};
	vkGetPhysicalDeviceProperties2(physicalDevice, &properties2);
	std::printf("fragmentDensityMap=%u dynamic=%u nonSubsampledImages=%u texelSize=%ux%u..%ux%u\n",
				density.fragmentDensityMap, density.fragmentDensityMapDynamic,
				density.fragmentDensityMapNonSubsampledImages, limits.minFragmentDensityTexelSize.width,
				limits.minFragmentDensityTexelSize.height, limits.maxFragmentDensityTexelSize.width,
				limits.maxFragmentDensityTexelSize.height);
	VkFormatProperties format{};
	vkGetPhysicalDeviceFormatProperties(physicalDevice, VK_FORMAT_R8G8_UNORM, &format);
	VkImageFormatProperties image{};
	const auto imageResult = vkGetPhysicalDeviceImageFormatProperties(
		physicalDevice, VK_FORMAT_R8G8_UNORM, VK_IMAGE_TYPE_2D, VK_IMAGE_TILING_OPTIMAL,
		VK_IMAGE_USAGE_FRAGMENT_DENSITY_MAP_BIT_EXT | VK_IMAGE_USAGE_TRANSFER_DST_BIT, 0, &image);
	if (imageResult != VK_SUCCESS && imageResult != VK_ERROR_FORMAT_NOT_SUPPORTED)
		Check(imageResult, "query density-map image support");
	if (!density.fragmentDensityMap ||
		!(format.optimalTilingFeatures & VK_FORMAT_FEATURE_FRAGMENT_DENSITY_MAP_BIT_EXT) || imageResult != VK_SUCCESS ||
		image.maxArrayLayers < 2 || image.maxExtent.width < 32 || image.maxExtent.height < 32)
	{
		std::puts("fbVulkanDensityMap=unavailable reason=required-feature-or-stereo-image-format-unsupported");
		return false;
	}
	DensityMap allocation;
	allocation.Create(physicalDevice, density);
	return true;
}
} // namespace simxr::probe

int main(int argc, char** argv)
{
	using namespace simxr::probe;
	std::setvbuf(stdout, nullptr, _IONBF, 0);
	const bool requireDensity = argc == 2 && std::strcmp(argv[1], "--require-density-map") == 0;
	if (argc != 1 && !requireDensity)
	{
		std::fputs("Usage: foveation_capabilities_probe [--require-density-map]\n", stderr);
		return 2;
	}
	try
	{
		bool allocationQualified = false;
		bool deviceAvailable = false;
		{
			Instance instance;
			instance.Create();
			std::uint32_t count = 0;
			Check(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "enumerate foveation devices");
			if (count)
			{
				std::vector<VkPhysicalDevice> devices(count);
				Check(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()),
					  "enumerate foveation devices");
				devices.resize(count);
				deviceAvailable = !devices.empty();
				for (const auto device : devices)
					allocationQualified |= Inspect(device);
			}
		}
		if (s_validationErrors.load())
			throw std::runtime_error("Vulkan validation reported foveation allocation errors");
		if (!deviceAvailable)
		{
			std::puts("SKIP: no Vulkan physical device is available for capability inspection");
			return 77;
		}
		if (requireDensity && !allocationQualified)
		{
			std::puts("SKIP: no device supports the density-map allocation prerequisite; rendering capability remains "
					  "disabled");
			return 77;
		}
		std::puts("PASS: queried actual device capabilities; no FB rendering support is inferred from capability "
				  "enumeration");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
