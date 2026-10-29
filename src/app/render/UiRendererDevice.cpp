#include "app/UiPch.h"
#include "app/widgets/Theme.h"
#include "app/platform/Window.h"
#include "app/render/UiRenderResources.h"
#include "app/render/UiRenderer.h"
#include "app/services/Utf8Path.h"
#include "graphics/backends/vulkan/VulkanExternal.h"
#include <SDL3/SDL_vulkan.h>
#include <imgui_impl_sdl3.h>

namespace simxr::app
{
static VKAPI_ATTR VkBool32 VKAPI_CALL Report(VkDebugUtilsMessageSeverityFlagBitsEXT severity,
											 VkDebugUtilsMessageTypeFlagsEXT,
											 const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		static_cast<ValidationLog*>(user)->errors.fetch_add(1);
	std::fprintf(stderr, "Companion Vulkan: %s\n", data->pMessage);
	return VK_FALSE;
}

void UiRenderer::Initialize(Window& owner, bool enableValidation, std::shared_ptr<ValidationLog> log,
							bool enableScreenshots, bool proofConsumer)
{
	auto& gpu = *m_resources;
	m_window = &owner;
	m_screenshots = enableScreenshots;
	if (enableValidation && !log)
		throw std::invalid_argument("Vulkan validation requires a retained log owner");
	gpu.validation = std::move(log);
	std::uint32_t extensionCount = 0;
	const auto required = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
	if (!required)
		throw std::runtime_error(SDL_GetError());
	std::vector<const char*> extensions(required, required + extensionCount);
	const char* layer = "VK_LAYER_KHRONOS_validation";
	VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
	debug.messageSeverity =
		VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
	debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
						VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
	debug.pfnUserCallback = Report;
	debug.pUserData = gpu.validation.get();
	VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
	application.pApplicationName = "OpenXR Simulator";
	application.apiVersion = VK_API_VERSION_1_1;
	VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
	if (enableValidation)
	{
		std::uint32_t count = 0;
		CheckResult(vkEnumerateInstanceLayerProperties(&count, nullptr));
		std::vector<VkLayerProperties> layers(count);
		CheckResult(vkEnumerateInstanceLayerProperties(&count, layers.data()));
		if (std::none_of(layers.begin(), layers.end(),
						 [&](const auto& value) { return std::strcmp(value.layerName, layer) == 0; }))
			throw std::runtime_error("Requested Vulkan validation layer is unavailable");
		extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
		info.enabledLayerCount = 1;
		info.ppEnabledLayerNames = &layer;
		info.pNext = &debug;
	}
	info.pApplicationInfo = &application;
	info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
	info.ppEnabledExtensionNames = extensions.data();
	const auto instanceResult = vkCreateInstance(&info, nullptr, &gpu.instance);
	if (instanceResult == VK_ERROR_INCOMPATIBLE_DRIVER)
		throw std::runtime_error("Vulkan could not find a compatible graphics driver.\n\n"
								 "Install or update the graphics driver for your GPU, then try again.");
	CheckResult(instanceResult);
	if (enableValidation)
	{
		auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
			vkGetInstanceProcAddr(gpu.instance, "vkCreateDebugUtilsMessengerEXT"));
		if (!create)
			throw std::runtime_error("Vulkan debug messenger unavailable");
		CheckResult(create(gpu.instance, &debug, nullptr, &gpu.messenger));
	}
	if (!SDL_Vulkan_CreateSurface(m_window->Native(), gpu.instance, nullptr, &gpu.surface))
		throw std::runtime_error(SDL_GetError());
	std::uint32_t count = 0;
	CheckResult(vkEnumeratePhysicalDevices(gpu.instance, &count, nullptr));
	std::vector<VkPhysicalDevice> devices(count);
	CheckResult(vkEnumeratePhysicalDevices(gpu.instance, &count, devices.data()));
	for (const auto candidate : devices)
	{
		std::uint32_t families = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
		std::vector<VkQueueFamilyProperties> properties(families);
		vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, properties.data());
		for (std::uint32_t index = 0; index < families; ++index)
		{
			VkBool32 present = VK_FALSE;
			CheckResult(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, gpu.surface, &present));
			if ((properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present &&
				properties[index].queueCount >= (proofConsumer ? 2u : 1u))
			{
				gpu.physical = candidate;
				gpu.family = index;
				break;
			}
		}
		if (gpu.physical)
			break;
	}
	if (!gpu.physical)
		throw std::runtime_error("No Vulkan graphics device can present to this display.\n\n"
								 "Check your graphics driver and display connection, then try again.");
	VkPhysicalDeviceProperties properties;
	vkGetPhysicalDeviceProperties(gpu.physical, &properties);
	m_adapter = properties.deviceName;
	const float priorities[]{1.0f, 1.0f};
	VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
	queueInfo.queueFamilyIndex = gpu.family;
	queueInfo.queueCount = proofConsumer ? 2 : 1;
	queueInfo.pQueuePriorities = priorities;
	std::vector<const char*> deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
	std::uint32_t deviceExtensionCount = 0;
	CheckResult(vkEnumerateDeviceExtensionProperties(gpu.physical, nullptr, &deviceExtensionCount, nullptr));
	std::vector<VkExtensionProperties> supportedExtensions(deviceExtensionCount);
	CheckResult(
		vkEnumerateDeviceExtensionProperties(gpu.physical, nullptr, &deviceExtensionCount, supportedExtensions.data()));

#if defined(__linux__) || defined(_WIN32)
	const auto supports = [&](const char* name) {
		return std::any_of(supportedExtensions.begin(), supportedExtensions.end(),
						   [&](const auto& value) { return std::strcmp(value.extensionName, name) == 0; });
	};
	std::uint32_t families = 0;
	vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &families, nullptr);
	std::vector<VkQueueFamilyProperties> familyProperties(families);
	vkGetPhysicalDeviceQueueFamilyProperties(gpu.physical, &families, familyProperties.data());
	gpu.externalImages = supports(VulkanExternalMemoryExtension) && supports(VulkanExternalSemaphoreExtension) &&
						 familyProperties[gpu.family].queueCount >= 2;
	if (gpu.externalImages || proofConsumer)
	{
		deviceExtensions.push_back(VulkanExternalMemoryExtension);
		deviceExtensions.push_back(VulkanExternalSemaphoreExtension);
		queueInfo.queueCount = 2;
	}
	if (gpu.externalImages && supports(VulkanExternalFenceExtension))
	{
		VkPhysicalDeviceExternalFenceInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
#ifdef _WIN32
		info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
		info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
		VkExternalFenceProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
		vkGetPhysicalDeviceExternalFenceProperties(gpu.physical, &info, &properties);
		constexpr auto flags = VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT;
		gpu.externalFence = (properties.externalFenceFeatures & flags) == flags;
		if (gpu.externalFence)
			deviceExtensions.push_back(VulkanExternalFenceExtension);
	}
	if (gpu.externalImages)
	{
		const auto caps = VulkanExternalDevice::QueryTimeline(gpu.physical, VK_API_VERSION_1_1);
		gpu.externalTimeline = caps.CanImportSemaphore() && caps.CanExportSemaphore();
		if (gpu.externalTimeline)
			deviceExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
	}
#endif
	for (const auto name : deviceExtensions)
		if (std::none_of(supportedExtensions.begin(), supportedExtensions.end(),
						 [&](const auto& value) { return std::strcmp(value.extensionName, name) == 0; }))
			throw std::runtime_error(std::string("Companion device lacks required extension: ") + name);
	VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature{
		VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
	timelineFeature.timelineSemaphore = gpu.externalTimeline;
	VkDeviceCreateInfo createDevice{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
	createDevice.pNext = gpu.externalTimeline ? &timelineFeature : nullptr;
	createDevice.queueCreateInfoCount = 1;
	createDevice.pQueueCreateInfos = &queueInfo;
	createDevice.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
	createDevice.ppEnabledExtensionNames = deviceExtensions.data();
	CheckResult(vkCreateDevice(gpu.physical, &createDevice, nullptr, &gpu.device));
	vkGetDeviceQueue(gpu.device, gpu.family, 0, &gpu.queue);
	if (queueInfo.queueCount >= 2)
		vkGetDeviceQueue(gpu.device, gpu.family, 1, &gpu.ioQueue);
	VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128};
	VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
	pool.maxSets = 128;
	pool.poolSizeCount = 1;
	pool.pPoolSizes = &size;
	CheckResult(vkCreateDescriptorPool(gpu.device, &pool, nullptr, &gpu.descriptors));
	for (auto& frame : gpu.frames)
	{
		VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		commands.queueFamilyIndex = gpu.family;
		CheckResult(vkCreateCommandPool(gpu.device, &commands, nullptr, &frame.pool));
		VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		allocate.commandPool = frame.pool;
		allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocate.commandBufferCount = 1;
		CheckResult(vkAllocateCommandBuffers(gpu.device, &allocate, &frame.command));
		VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		CheckResult(vkCreateSemaphore(gpu.device, &semaphore, nullptr, &frame.acquired));
		VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		CheckResult(vkCreateFence(gpu.device, &fence, nullptr, &frame.fence));
	}
	gpu.context = ImGui::CreateContext();
	ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
	ImGui::GetIO().IniFilename = nullptr;
	if (const auto base = SDL_GetBasePath())
	{
		const auto font = Utf8Path(base) / "fonts/AdwaitaSans-Regular.ttf";
		if (std::filesystem::exists(font))
		{
			// Adwaita Sans (an Inter fork) has proportional digits; they merge back at one width so live
			// numbers do not jitter. Clamped glyphs are centred in their advance.
			static constexpr ImWchar digits[]{'0', '9', 0};
			ImFontConfig text;
			text.GlyphExcludeRanges = digits;
			auto& fonts = *ImGui::GetIO().Fonts;
			fonts.AddFontFromFileTTF(PathText(font).c_str(), 17, &text);
			ImFontConfig figures;
			figures.MergeMode = true;
			figures.GlyphRanges = digits;
			figures.GlyphMinAdvanceX = figures.GlyphMaxAdvanceX = 17 * .6f;
			fonts.AddFontFromFileTTF(PathText(font).c_str(), 17, &figures);
		}
	}
	auto& style = ImGui::GetStyle();
	theme::Apply(style);
	style.FontSizeBase = 17;
	style.ScaleAllSizes(SDL_GetWindowDisplayScale(m_window->Native()));
	if (!ImGui_ImplSDL3_InitForVulkan(m_window->Native()))
		throw std::runtime_error("ImGui SDL initialization failed");

	CreateSwapchain();
}
} // namespace simxr::app
