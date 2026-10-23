#include "app/UiRenderer.h"
#include "CompanionSpirv.h"
#include "app/Theme.h"
#include "app/Window.h"
#include "graphics/backends/vulkan/VulkanExternal.h"

#include <SDL3/SDL.h>
#include <SDL3/SDL_vulkan.h>
#include <algorithm>
#include <array>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <imgui.h>

#include <ImGuizmo.h>

#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>
#include <limits>
#include <stdexcept>
#include <vector>
#include <vulkan/vulkan.h>

namespace simxr::app
{
namespace
{
void Check(VkResult result)
{
	if (result != VK_SUCCESS)
		throw std::runtime_error("Vulkan operation failed: " + std::to_string(result));
}

bool IsSrgb(VkFormat format)
{
	return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

VKAPI_ATTR VkBool32 VKAPI_CALL Report(VkDebugUtilsMessageSeverityFlagBitsEXT severity, VkDebugUtilsMessageTypeFlagsEXT,
									  const VkDebugUtilsMessengerCallbackDataEXT* data, void* user)
{
	if (severity & VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT)
		static_cast<ValidationLog*>(user)->errors.fetch_add(1);
	std::fprintf(stderr, "Companion Vulkan: %s\n", data->pMessage);
	return VK_FALSE;
}
} // namespace

struct UiRenderer::Impl
{
	struct Frame
	{
		VkCommandPool pool = VK_NULL_HANDLE;
		VkCommandBuffer command = VK_NULL_HANDLE;
		VkSemaphore acquired = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
		bool hasImage = false;
		std::uint32_t image = 0;
		std::uint64_t serial = 0;
	};
	Window* window = nullptr;
	std::shared_ptr<ValidationLog> validation;
	VkInstance instance = VK_NULL_HANDLE;
	VkDebugUtilsMessengerEXT messenger = VK_NULL_HANDLE;
	VkSurfaceKHR surface = VK_NULL_HANDLE;
	VkPhysicalDevice physical = VK_NULL_HANDLE;
	VkDevice device = VK_NULL_HANDLE;
	VkQueue queue = VK_NULL_HANDLE;
	VkQueue ioQueue = VK_NULL_HANDLE;
	bool externalImages = false, externalFence = false, externalTimeline = false;
	std::uint32_t family = 0;
	VkDescriptorPool descriptors = VK_NULL_HANDLE;
	VkSwapchainKHR swapchain = VK_NULL_HANDLE;
	VkRenderPass renderPass = VK_NULL_HANDLE;
	VkFormat format = VK_FORMAT_UNDEFINED;
	VkExtent2D extent{};
	std::vector<VkImage> swapchainImages;
	std::vector<VkImageView> views;
	std::vector<VkFramebuffer> framebuffers;
	std::vector<VkSemaphore> rendered;
	std::vector<VkFence> imageFences;
	std::array<Frame, 2> frames;
	std::size_t frameSlot = 0;
	ImGuiContext* context = nullptr;
	bool platformReady = false;
	bool rendererReady = false;
	bool m_frameStarted = false;
	bool rebuild = false;
	std::string adapter;
	std::uint64_t submitted = 0;
	std::uint64_t completed = 0;
	std::uint32_t creations = 0;
	bool screenshots = false;
	std::string screenshotPath;
	VkBuffer readback = VK_NULL_HANDLE;
	VkDeviceMemory readbackMemory = VK_NULL_HANDLE;

	~Impl()
	{
		if (device)
		{
			// A returned acquisition can still need its semaphore consumed if the
			// window closes before this CPU frame reaches command submission.
			for (auto& frame : frames)
				if (frame.hasImage)
				{
					const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
					VkSubmitInfo consume{VK_STRUCTURE_TYPE_SUBMIT_INFO};
					consume.waitSemaphoreCount = 1;
					consume.pWaitSemaphores = &frame.acquired;
					consume.pWaitDstStageMask = &stage;
					const auto consumed = vkQueueSubmit(queue, 1, &consume, VK_NULL_HANDLE);
					if (consumed != VK_SUCCESS)
					{
						std::fprintf(stderr, "Companion shutdown could not retire acquisition: %d\n", consumed);
						std::_Exit(1);
					}
				}
			const auto idle = vkDeviceWaitIdle(device);
			if (idle != VK_SUCCESS)
			{
				std::fprintf(stderr, "Companion shutdown completion remains unknown: %d\n", idle);
				std::_Exit(1);
			}
		}
		if (context)
			ImGui::SetCurrentContext(context);
		if (context && ImGui::GetIO().BackendRendererUserData)
			ImGui_ImplVulkan_Shutdown();
		if (context && ImGui::GetIO().BackendPlatformUserData)
			ImGui_ImplSDL3_Shutdown();
		if (context)
			ImGui::DestroyContext(context);
		DestroySwapchain();
		if (device)
		{
			for (auto& frame : frames)
			{
				vkDestroyFence(device, frame.fence, nullptr);
				vkDestroySemaphore(device, frame.acquired, nullptr);
				vkDestroyCommandPool(device, frame.pool, nullptr);
			}
			vkDestroyBuffer(device, readback, nullptr);
			vkFreeMemory(device, readbackMemory, nullptr);
			vkDestroyDescriptorPool(device, descriptors, nullptr);
			vkDestroyDevice(device, nullptr);
		}
		if (surface)
			vkDestroySurfaceKHR(instance, surface, nullptr);
		if (messenger)
		{
			auto destroy = reinterpret_cast<PFN_vkDestroyDebugUtilsMessengerEXT>(
				vkGetInstanceProcAddr(instance, "vkDestroyDebugUtilsMessengerEXT"));
			destroy(instance, messenger, nullptr);
		}
		if (instance)
			vkDestroyInstance(instance, nullptr);
	}

	void Initialize(Window& owner, bool enableValidation, std::shared_ptr<ValidationLog> log, bool enableScreenshots,
					bool proofConsumer)
	{
		window = &owner;
		screenshots = enableScreenshots;
		validation = std::move(log);
		std::uint32_t extensionCount = 0;
		const auto required = SDL_Vulkan_GetInstanceExtensions(&extensionCount);
		if (!required)
			throw std::runtime_error(SDL_GetError());
		std::vector<const char*> extensions(required, required + extensionCount);
		const char* layer = "VK_LAYER_KHRONOS_validation";
		VkDebugUtilsMessengerCreateInfoEXT debug{VK_STRUCTURE_TYPE_DEBUG_UTILS_MESSENGER_CREATE_INFO_EXT};
		debug.messageSeverity =
			VK_DEBUG_UTILS_MESSAGE_SEVERITY_ERROR_BIT_EXT | VK_DEBUG_UTILS_MESSAGE_SEVERITY_WARNING_BIT_EXT;
		debug.messageType = VK_DEBUG_UTILS_MESSAGE_TYPE_GENERAL_BIT_EXT |
							VK_DEBUG_UTILS_MESSAGE_TYPE_VALIDATION_BIT_EXT |
							VK_DEBUG_UTILS_MESSAGE_TYPE_PERFORMANCE_BIT_EXT;
		debug.pfnUserCallback = Report;
		debug.pUserData = validation.get();
		VkApplicationInfo application{VK_STRUCTURE_TYPE_APPLICATION_INFO};
		application.pApplicationName = "OpenXR Simulator Companion";
		application.apiVersion = VK_API_VERSION_1_1;
		VkInstanceCreateInfo info{VK_STRUCTURE_TYPE_INSTANCE_CREATE_INFO};
		if (enableValidation)
		{
			std::uint32_t count = 0;
			Check(vkEnumerateInstanceLayerProperties(&count, nullptr));
			std::vector<VkLayerProperties> layers(count);
			Check(vkEnumerateInstanceLayerProperties(&count, layers.data()));
			if (std::none_of(layers.begin(), layers.end(),
							 [&](const auto& value)
							 {
								 return std::strcmp(value.layerName, layer) == 0;
							 }))
				throw std::runtime_error("Requested Vulkan validation layer is unavailable");
			extensions.push_back(VK_EXT_DEBUG_UTILS_EXTENSION_NAME);
			info.enabledLayerCount = 1;
			info.ppEnabledLayerNames = &layer;
			info.pNext = &debug;
		}
		info.pApplicationInfo = &application;
		info.enabledExtensionCount = static_cast<std::uint32_t>(extensions.size());
		info.ppEnabledExtensionNames = extensions.data();
		Check(vkCreateInstance(&info, nullptr, &instance));
		if (enableValidation)
		{
			auto create = reinterpret_cast<PFN_vkCreateDebugUtilsMessengerEXT>(
				vkGetInstanceProcAddr(instance, "vkCreateDebugUtilsMessengerEXT"));
			if (!create)
				throw std::runtime_error("Vulkan debug messenger unavailable");
			Check(create(instance, &debug, nullptr, &messenger));
		}
		if (!SDL_Vulkan_CreateSurface(window->Native(), instance, nullptr, &surface))
			throw std::runtime_error(SDL_GetError());
		std::uint32_t count = 0;
		Check(vkEnumeratePhysicalDevices(instance, &count, nullptr));
		std::vector<VkPhysicalDevice> devices(count);
		Check(vkEnumeratePhysicalDevices(instance, &count, devices.data()));
		for (const auto candidate : devices)
		{
			std::uint32_t families = 0;
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, nullptr);
			std::vector<VkQueueFamilyProperties> properties(families);
			vkGetPhysicalDeviceQueueFamilyProperties(candidate, &families, properties.data());
			for (std::uint32_t index = 0; index < families; ++index)
			{
				VkBool32 present = VK_FALSE;
				Check(vkGetPhysicalDeviceSurfaceSupportKHR(candidate, index, surface, &present));
				if ((properties[index].queueFlags & VK_QUEUE_GRAPHICS_BIT) && present &&
					properties[index].queueCount >= (proofConsumer ? 2u : 1u))
				{
					physical = candidate;
					family = index;
					break;
				}
			}
			if (physical)
				break;
		}
		if (!physical)
			throw std::runtime_error("No Vulkan graphics/presentation queue for this SDL surface");
		VkPhysicalDeviceProperties properties;
		vkGetPhysicalDeviceProperties(physical, &properties);
		adapter = properties.deviceName;
		const float priorities[]{1.0f, 1.0f};
		VkDeviceQueueCreateInfo queueInfo{VK_STRUCTURE_TYPE_DEVICE_QUEUE_CREATE_INFO};
		queueInfo.queueFamilyIndex = family;
		queueInfo.queueCount = proofConsumer ? 2 : 1;
		queueInfo.pQueuePriorities = priorities;
		std::vector<const char*> deviceExtensions{VK_KHR_SWAPCHAIN_EXTENSION_NAME};
		std::uint32_t deviceExtensionCount = 0;
		Check(vkEnumerateDeviceExtensionProperties(physical, nullptr, &deviceExtensionCount, nullptr));
		std::vector<VkExtensionProperties> supportedExtensions(deviceExtensionCount);
		Check(
			vkEnumerateDeviceExtensionProperties(physical, nullptr, &deviceExtensionCount, supportedExtensions.data()));

#if defined(__linux__) || defined(_WIN32)
		const auto supports = [&](const char* name)
		{
			return std::any_of(supportedExtensions.begin(), supportedExtensions.end(),
							   [&](const auto& value)
							   {
								   return std::strcmp(value.extensionName, name) == 0;
							   });
		};
		std::uint32_t families = 0;
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, nullptr);
		std::vector<VkQueueFamilyProperties> familyProperties(families);
		vkGetPhysicalDeviceQueueFamilyProperties(physical, &families, familyProperties.data());
		externalImages = supports(VulkanExternalMemoryExtension) && supports(VulkanExternalSemaphoreExtension) &&
						 familyProperties[family].queueCount >= 2;
		if (externalImages || proofConsumer)
		{
			deviceExtensions.push_back(VulkanExternalMemoryExtension);
			deviceExtensions.push_back(VulkanExternalSemaphoreExtension);
			queueInfo.queueCount = 2;
		}
		if (externalImages && supports(VulkanExternalFenceExtension))
		{
			VkPhysicalDeviceExternalFenceInfo info{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_EXTERNAL_FENCE_INFO};
#ifdef _WIN32
			info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_WIN32_BIT;
#else
			info.handleType = VK_EXTERNAL_FENCE_HANDLE_TYPE_OPAQUE_FD_BIT;
#endif
			VkExternalFenceProperties properties{VK_STRUCTURE_TYPE_EXTERNAL_FENCE_PROPERTIES};
			vkGetPhysicalDeviceExternalFenceProperties(physical, &info, &properties);
			constexpr auto flags = VK_EXTERNAL_FENCE_FEATURE_IMPORTABLE_BIT | VK_EXTERNAL_FENCE_FEATURE_EXPORTABLE_BIT;
			externalFence = (properties.externalFenceFeatures & flags) == flags;
			if (externalFence)
				deviceExtensions.push_back(VulkanExternalFenceExtension);
		}
		if (externalImages)
		{
			const auto caps = VulkanExternalDevice::QueryTimeline(physical, VK_API_VERSION_1_1);
			externalTimeline = caps.CanImportSemaphore() && caps.CanExportSemaphore();
			if (externalTimeline)
				deviceExtensions.push_back(VK_KHR_TIMELINE_SEMAPHORE_EXTENSION_NAME);
		}
#endif
		for (const auto name : deviceExtensions)
			if (std::none_of(supportedExtensions.begin(), supportedExtensions.end(),
							 [&](const auto& value)
							 {
								 return std::strcmp(value.extensionName, name) == 0;
							 }))
				throw std::runtime_error(std::string("Companion device lacks required extension: ") + name);
		VkPhysicalDeviceTimelineSemaphoreFeatures timelineFeature{
			VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_TIMELINE_SEMAPHORE_FEATURES};
		timelineFeature.timelineSemaphore = externalTimeline;
		VkDeviceCreateInfo createDevice{VK_STRUCTURE_TYPE_DEVICE_CREATE_INFO};
		createDevice.pNext = externalTimeline ? &timelineFeature : nullptr;
		createDevice.queueCreateInfoCount = 1;
		createDevice.pQueueCreateInfos = &queueInfo;
		createDevice.enabledExtensionCount = static_cast<std::uint32_t>(deviceExtensions.size());
		createDevice.ppEnabledExtensionNames = deviceExtensions.data();
		Check(vkCreateDevice(physical, &createDevice, nullptr, &device));
		vkGetDeviceQueue(device, family, 0, &queue);
		if (queueInfo.queueCount >= 2)
			vkGetDeviceQueue(device, family, 1, &ioQueue);
		VkDescriptorPoolSize size{VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 128};
		VkDescriptorPoolCreateInfo pool{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
		pool.flags = VK_DESCRIPTOR_POOL_CREATE_FREE_DESCRIPTOR_SET_BIT;
		pool.maxSets = 128;
		pool.poolSizeCount = 1;
		pool.pPoolSizes = &size;
		Check(vkCreateDescriptorPool(device, &pool, nullptr, &descriptors));
		for (auto& frame : frames)
		{
			VkCommandPoolCreateInfo commands{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
			commands.queueFamilyIndex = family;
			Check(vkCreateCommandPool(device, &commands, nullptr, &frame.pool));
			VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
			allocate.commandPool = frame.pool;
			allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
			allocate.commandBufferCount = 1;
			Check(vkAllocateCommandBuffers(device, &allocate, &frame.command));
			VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			Check(vkCreateSemaphore(device, &semaphore, nullptr, &frame.acquired));
			VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
			fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
			Check(vkCreateFence(device, &fence, nullptr, &frame.fence));
		}
		context = ImGui::CreateContext();
		ImGui::GetIO().ConfigFlags |= ImGuiConfigFlags_NavEnableKeyboard;
		ImGui::GetIO().IniFilename = nullptr;
		if (const auto base = SDL_GetBasePath())
		{
			const auto font = std::filesystem::path(base) / "fonts/AdwaitaSans-Regular.ttf";
			if (std::filesystem::exists(font))
			{
				// Adwaita Sans (an Inter fork) has proportional digits; they merge back at one width so live
				// numbers do not jitter. Clamped glyphs are centred in their advance.
				static constexpr ImWchar digits[]{'0', '9', 0};
				ImFontConfig text;
				text.GlyphExcludeRanges = digits;
				auto& fonts = *ImGui::GetIO().Fonts;
				fonts.AddFontFromFileTTF(font.string().c_str(), 17, &text);
				ImFontConfig figures;
				figures.MergeMode = true;
				figures.GlyphRanges = digits;
				figures.GlyphMinAdvanceX = figures.GlyphMaxAdvanceX = 17 * .6f;
				fonts.AddFontFromFileTTF(font.string().c_str(), 17, &figures);
			}
		}
		auto& style = ImGui::GetStyle();
		theme::Apply(style);
		style.FontSizeBase = 17;
		style.ScaleAllSizes(SDL_GetWindowDisplayScale(window->Native()));
		if (!ImGui_ImplSDL3_InitForVulkan(window->Native()))
			throw std::runtime_error("ImGui SDL initialization failed");
		platformReady = true;
		CreateSwapchain();
	}

	void DestroySwapchain()
	{
		if (!device)
			return;
		for (auto framebuffer : framebuffers)
			vkDestroyFramebuffer(device, framebuffer, nullptr);
		for (auto view : views)
			vkDestroyImageView(device, view, nullptr);
		for (auto semaphore : rendered)
			vkDestroySemaphore(device, semaphore, nullptr);
		vkDestroyRenderPass(device, renderPass, nullptr);
		vkDestroySwapchainKHR(device, swapchain, nullptr);
		framebuffers.clear();
		views.clear();
		rendered.clear();
		imageFences.clear();
		renderPass = VK_NULL_HANDLE;
		swapchain = VK_NULL_HANDLE;
	}

	void CreateSwapchain()
	{
		const auto [width, height] = window->PixelSize();
		if (width <= 0 || height <= 0)
			return;
		// The IO queue may be waiting on an external producer. Resizing only
		// retires UI work and must not wait for that independent queue.
		Check(vkQueueWaitIdle(queue));
		completed = submitted;
		DestroySwapchain();
		VkSurfaceCapabilitiesKHR capabilities;
		Check(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(physical, surface, &capabilities));
		std::uint32_t count = 0;
		Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, nullptr));
		std::vector<VkSurfaceFormatKHR> formats(count);
		Check(vkGetPhysicalDeviceSurfaceFormatsKHR(physical, surface, &count, formats.data()));
		if (formats.empty())
			throw std::runtime_error("Surface has no Vulkan color format");
		auto selected = formats.front();
		if (selected.format == VK_FORMAT_UNDEFINED)
			selected.format = VK_FORMAT_B8G8R8A8_UNORM;
		for (const auto& value : formats)
			if (value.format == VK_FORMAT_B8G8R8A8_UNORM && value.colorSpace == VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
				selected = value;
		if (selected.colorSpace != VK_COLOR_SPACE_SRGB_NONLINEAR_KHR)
			throw std::runtime_error("The companion requires an sRGB display surface");
		if (rendererReady && IsSrgb(format) != IsSrgb(selected.format))
			throw std::runtime_error("Display color encoding changed; reopen the companion for the new surface");
		format = selected.format;
		extent = capabilities.currentExtent;
		if (extent.width == std::numeric_limits<std::uint32_t>::max())
			extent = {std::clamp(static_cast<std::uint32_t>(width), capabilities.minImageExtent.width,
								 capabilities.maxImageExtent.width),
					  std::clamp(static_cast<std::uint32_t>(height), capabilities.minImageExtent.height,
								 capabilities.maxImageExtent.height)};
		std::uint32_t imageCount = std::max(2u, capabilities.minImageCount + 1);
		if (capabilities.maxImageCount)
			imageCount = std::min(imageCount, capabilities.maxImageCount);
		if (imageCount < 2 || !(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT))
			throw std::runtime_error("Surface cannot provide ImGui color-attachment swapchain");
		VkCompositeAlphaFlagBitsKHR alpha = VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR;
		for (auto candidate : {VK_COMPOSITE_ALPHA_OPAQUE_BIT_KHR, VK_COMPOSITE_ALPHA_PRE_MULTIPLIED_BIT_KHR,
							   VK_COMPOSITE_ALPHA_POST_MULTIPLIED_BIT_KHR, VK_COMPOSITE_ALPHA_INHERIT_BIT_KHR})
			if (capabilities.supportedCompositeAlpha & candidate)
			{
				alpha = candidate;
				break;
			}
		VkSwapchainCreateInfoKHR create{VK_STRUCTURE_TYPE_SWAPCHAIN_CREATE_INFO_KHR};
		create.surface = surface;
		create.minImageCount = imageCount;
		create.imageFormat = format;
		create.imageColorSpace = selected.colorSpace;
		create.imageExtent = extent;
		create.imageArrayLayers = 1;
		create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
		if (screenshots)
		{
			if (!(capabilities.supportedUsageFlags & VK_IMAGE_USAGE_TRANSFER_SRC_BIT))
				throw std::runtime_error("Explicit screenshot unavailable: surface lacks TRANSFER_SRC usage");
			create.imageUsage |= VK_IMAGE_USAGE_TRANSFER_SRC_BIT;
		}
		create.imageSharingMode = VK_SHARING_MODE_EXCLUSIVE;
		create.preTransform = capabilities.currentTransform;
		create.compositeAlpha = alpha;
		create.presentMode = VK_PRESENT_MODE_FIFO_KHR;
		create.clipped = VK_TRUE;
		Check(vkCreateSwapchainKHR(device, &create, nullptr, &swapchain));
		Check(vkGetSwapchainImagesKHR(device, swapchain, &count, nullptr));
		std::vector<VkImage> images(count);
		Check(vkGetSwapchainImagesKHR(device, swapchain, &count, images.data()));
		swapchainImages = images;
		VkAttachmentDescription attachment{};
		attachment.format = format;
		attachment.samples = VK_SAMPLE_COUNT_1_BIT;
		attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
		attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
		attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
		attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
		attachment.initialLayout = VK_IMAGE_LAYOUT_UNDEFINED;
		attachment.finalLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		VkAttachmentReference color{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
		VkSubpassDescription subpass{};
		subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
		subpass.colorAttachmentCount = 1;
		subpass.pColorAttachments = &color;
		VkSubpassDependency dependency{};
		dependency.srcSubpass = VK_SUBPASS_EXTERNAL;
		dependency.dstSubpass = 0;
		dependency.srcStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstStageMask = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		dependency.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
		pass.attachmentCount = 1;
		pass.pAttachments = &attachment;
		pass.subpassCount = 1;
		pass.pSubpasses = &subpass;
		pass.dependencyCount = 1;
		pass.pDependencies = &dependency;
		Check(vkCreateRenderPass(device, &pass, nullptr, &renderPass));
		views.resize(count);
		framebuffers.resize(count);
		rendered.resize(count);
		imageFences.assign(count, VK_NULL_HANDLE);
		for (std::uint32_t index = 0; index < count; ++index)
		{
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = images[index];
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = format;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
			Check(vkCreateImageView(device, &view, nullptr, &views[index]));
			VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			framebuffer.renderPass = renderPass;
			framebuffer.attachmentCount = 1;
			framebuffer.pAttachments = &views[index];
			framebuffer.width = extent.width;
			framebuffer.height = extent.height;
			framebuffer.layers = 1;
			Check(vkCreateFramebuffer(device, &framebuffer, nullptr, &framebuffers[index]));
			VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
			Check(vkCreateSemaphore(device, &semaphore, nullptr, &rendered[index]));
		}
		ImGui_ImplVulkan_PipelineInfo pipeline{};
		pipeline.RenderPass = renderPass;
		pipeline.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
		if (rendererReady)
			ImGui_ImplVulkan_CreateMainPipeline(&pipeline);
		else
		{
			ImGui_ImplVulkan_InitInfo backend{};
			backend.ApiVersion = VK_API_VERSION_1_1;
			backend.CheckVkResultFn = Check;
			backend.Instance = instance;
			backend.PhysicalDevice = physical;
			backend.Device = device;
			backend.QueueFamily = family;
			backend.Queue = queue;
			backend.DescriptorPool = descriptors;
			// ImGui's upload ring follows our in-flight submissions, not the
			// swapchain image count. Keep both backends alive across resizes:
			// Vulkan backend shutdown also destroys platform viewport handles.
			backend.MinImageCount = static_cast<std::uint32_t>(frames.size());
			backend.ImageCount = static_cast<std::uint32_t>(frames.size());
			backend.PipelineInfoMain = pipeline;
			const bool srgb = IsSrgb(format);
			backend.CustomShaderFragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
			backend.CustomShaderFragCreateInfo.codeSize = srgb ? sizeof(shader::srgb) : sizeof(shader::unorm);
			backend.CustomShaderFragCreateInfo.pCode = srgb ? shader::srgb.data() : shader::unorm.data();
			if (!ImGui_ImplVulkan_Init(&backend))
				throw std::runtime_error("ImGui Vulkan initialization failed");
			rendererReady = true;
		}
		rebuild = false;
		++creations;
	}

	void PrepareReadback()
	{
		if (format != VK_FORMAT_B8G8R8A8_UNORM && format != VK_FORMAT_B8G8R8A8_SRGB &&
			format != VK_FORMAT_R8G8B8A8_UNORM && format != VK_FORMAT_R8G8B8A8_SRGB)
			throw std::runtime_error("Explicit screenshot requires an 8-bit RGBA/BGRA surface format");
		vkDestroyBuffer(device, readback, nullptr);
		readback = VK_NULL_HANDLE;
		vkFreeMemory(device, readbackMemory, nullptr);
		readbackMemory = VK_NULL_HANDLE;
		VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		create.size = static_cast<VkDeviceSize>(extent.width) * extent.height * 4;
		create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		Check(vkCreateBuffer(device, &create, nullptr, &readback));
		VkMemoryRequirements requirements;
		vkGetBufferMemoryRequirements(device, readback, &requirements);
		VkPhysicalDeviceMemoryProperties properties;
		vkGetPhysicalDeviceMemoryProperties(physical, &properties);
		std::uint32_t type = properties.memoryTypeCount;
		for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
			if ((requirements.memoryTypeBits & (1u << index)) &&
				(properties.memoryTypes[index].propertyFlags &
				 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
					(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
			{
				type = index;
				break;
			}
		if (type == properties.memoryTypeCount)
			throw std::runtime_error("Screenshot has no coherent host memory type");
		VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = type;
		Check(vkAllocateMemory(device, &allocate, nullptr, &readbackMemory));
		Check(vkBindBufferMemory(device, readback, readbackMemory, 0));
	}

	void RecordReadback(Frame& frame)
	{
		VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		image.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
		image.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		image.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		image.image = swapchainImages[frame.image];
		image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
							 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0, nullptr, 1, &image);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = {extent.width, extent.height, 1};
		vkCmdCopyImageToBuffer(frame.command, image.image, image.newLayout, readback, 1, &copy);
		image.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
		image.dstAccessMask = 0;
		image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
		image.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
		VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
		buffer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		buffer.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		buffer.srcQueueFamilyIndex = buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		buffer.buffer = readback;
		buffer.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(frame.command, VK_PIPELINE_STAGE_TRANSFER_BIT,
							 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1,
							 &buffer, 1, &image);
	}

	void WriteScreenshot()
	{
		const auto byteCount = static_cast<std::size_t>(extent.width) * extent.height * 4;
		void* mapped = nullptr;
		Check(vkMapMemory(device, readbackMemory, 0, byteCount, 0, &mapped));
		std::vector<unsigned char> pixels;
		try
		{
			pixels.assign(static_cast<unsigned char*>(mapped), static_cast<unsigned char*>(mapped) + byteCount);
		}
		catch (...)
		{
			vkUnmapMemory(device, readbackMemory);
			throw;
		}
		vkUnmapMemory(device, readbackMemory);
		if (format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB)
			for (std::size_t index = 0; index < pixels.size(); index += 4)
				std::swap(pixels[index], pixels[index + 2]);
		std::array<unsigned char, 54> header{};
		header[0] = 'B';
		header[1] = 'M';
		auto write = [&](std::size_t offset, std::uint32_t value)
		{
			for (unsigned index = 0; index < 4; ++index)
				header[offset + index] = static_cast<unsigned char>(value >> (8 * index));
		};
		write(2, static_cast<std::uint32_t>(54 + byteCount));
		write(10, 54);
		write(14, 40);
		write(18, extent.width);
		write(22, 0u - extent.height);
		header[26] = 1;
		header[28] = 32;
		std::ofstream file(screenshotPath, std::ios::binary);
		file.exceptions(std::ios::failbit | std::ios::badbit);
		file.write(reinterpret_cast<const char*>(header.data()), header.size());
		file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
		file.close();
		std::printf("Explicit UI screenshot: %s (%u x %u)\n", screenshotPath.c_str(), extent.width, extent.height);
		screenshotPath.clear();
	}

	bool BeginFrame()
	{
		if (m_frameStarted)
			throw std::logic_error("The admitted UI frame must be rendered before beginning another");
		ImGui::SetCurrentContext(context);
		const auto [width, height] = window->PixelSize();
		const bool acquired = std::any_of(frames.begin(), frames.end(),
										  [](const auto& frame)
										  {
											  return frame.hasImage;
										  });
		if (!acquired)
		{
			if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(window->Native()) & SDL_WINDOW_MINIMIZED))
				return false;
			if (rebuild || extent.width != static_cast<std::uint32_t>(width) ||
				extent.height != static_cast<std::uint32_t>(height))
				CreateSwapchain();
		}
		if (!rendererReady)
			return false;
		// Keep SDL/ImGui events queued until this frame has a usable image. An
		// unseen UI frame must not consume clicks, popup state or docking edits.
		auto& frame = frames[frameSlot];
		if (!frame.hasImage)
		{
			const auto ready = vkWaitForFences(device, 1, &frame.fence, VK_TRUE, 0);
			if (ready == VK_TIMEOUT)
				return false;
			Check(ready);
			completed = std::max(completed, frame.serial);
			const auto acquired =
				vkAcquireNextImageKHR(device, swapchain, 0, frame.acquired, VK_NULL_HANDLE, &frame.image);
			if (acquired == VK_NOT_READY || acquired == VK_TIMEOUT)
				return false;
			if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
			{
				rebuild = true;
				return false;
			}
			if (acquired == VK_SUBOPTIMAL_KHR)
				rebuild = true;
			else
				Check(acquired);
			frame.hasImage = true;
		}
		if (imageFences[frame.image])
		{
			const auto ready = vkWaitForFences(device, 1, &imageFences[frame.image], VK_TRUE, 0);
			if (ready == VK_TIMEOUT)
				return false;
			Check(ready);
		}
		ImGui_ImplVulkan_NewFrame();
		ImGui_ImplSDL3_NewFrame();
		ImGui::NewFrame();
		ImGuizmo::BeginFrame();
		m_frameStarted = true;
		return true;
	}

	bool Render(std::span<const VkSemaphore> readyWaits, const std::function<void(VkCommandBuffer)>& transfers)
	{
		if (!m_frameStarted)
			throw std::logic_error("UI rendering requires an admitted frame");
		m_frameStarted = false;
		ImGui::Render();
		auto& frame = frames[frameSlot];
		const bool capture = !screenshotPath.empty();
		if (capture)
			PrepareReadback();
		Check(vkResetCommandPool(device, frame.pool, 0));
		VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
		begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
		Check(vkBeginCommandBuffer(frame.command, &begin));
		if (transfers)
			transfers(frame.command);
		VkClearValue clear{};
		clear.color = {{0.04f, 0.05f, 0.07f, 1}};
		VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		pass.renderPass = renderPass;
		pass.framebuffer = framebuffers[frame.image];
		pass.renderArea.extent = extent;
		pass.clearValueCount = 1;
		pass.pClearValues = &clear;
		vkCmdBeginRenderPass(frame.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
		ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.command);
		vkCmdEndRenderPass(frame.command);
		if (capture)
			RecordReadback(frame);
		Check(vkEndCommandBuffer(frame.command));
		std::vector<VkSemaphore> waits{frame.acquired};
		waits.insert(waits.end(), readyWaits.begin(), readyWaits.end());
		// Ready images are sampled by the UI pass or copied before it.
		std::vector<VkPipelineStageFlags> stages(waits.size(),
												 VK_PIPELINE_STAGE_FRAGMENT_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT);
		stages[0] = VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT;
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.waitSemaphoreCount = static_cast<std::uint32_t>(waits.size());
		submit.pWaitSemaphores = waits.data();
		submit.pWaitDstStageMask = stages.data();
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &frame.command;
		submit.signalSemaphoreCount = 1;
		submit.pSignalSemaphores = &rendered[frame.image];
		Check(vkResetFences(device, 1, &frame.fence));
		const auto accepted = vkQueueSubmit(queue, 1, &submit, frame.fence);
		// OOM guarantees rejection. Other failures may leave native work in
		// flight: expose its serial before throwing so panel retirement cannot
		// free descriptors/private images referenced by that submission.
		if (accepted != VK_ERROR_OUT_OF_HOST_MEMORY && accepted != VK_ERROR_OUT_OF_DEVICE_MEMORY)
		{
			frame.serial = ++submitted;
			frame.hasImage = false;
		}
		Check(accepted);
		frame.hasImage = false;
		imageFences[frame.image] = frame.fence;
		VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
		present.waitSemaphoreCount = 1;
		present.pWaitSemaphores = &rendered[frame.image];
		present.swapchainCount = 1;
		present.pSwapchains = &swapchain;
		present.pImageIndices = &frame.image;
		const auto result = vkQueuePresentKHR(queue, &present);
		if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
			rebuild = true;
		else
			Check(result);
		if (capture)
		{
			Check(vkWaitForFences(device, 1, &frame.fence, VK_TRUE, 5000000000ULL));
			WriteScreenshot();
		}
		frameSlot = (frameSlot + 1) % frames.size();
		return true;
	}
};

UiRenderer::UiRenderer(Window& window, bool validation, std::shared_ptr<ValidationLog> log, bool screenshots,
					   bool proofConsumer)
	: m_impl(std::make_shared<Impl>())
{
	m_impl->Initialize(window, validation, std::move(log), screenshots, proofConsumer);
}
UiRenderer::~UiRenderer() = default;
bool UiRenderer::BeginFrame()
{
	return m_impl->BeginFrame();
}
bool UiRenderer::Render(std::span<const VkSemaphore> readyWaits,
						const std::function<void(VkCommandBuffer)>& transfers)
{
	return m_impl->Render(readyWaits, transfers);
}
VulkanDeviceAccess UiRenderer::DeviceAccess() const
{
	return {m_impl->instance,		m_impl->physical,	   m_impl->device, m_impl->family,			m_impl->ioQueue,
			m_impl->externalImages, m_impl->externalFence, m_impl,		   m_impl->externalTimeline};
}
std::uint64_t UiRenderer::CompletedFrames()
{
	for (const auto& frame : m_impl->frames)
		if (frame.serial > m_impl->completed)
		{
			const auto result = vkGetFenceStatus(m_impl->device, frame.fence);
			if (result == VK_SUCCESS)
				m_impl->completed = std::max(m_impl->completed, frame.serial);
			else if (result != VK_NOT_READY)
				Check(result);
		}
	return m_impl->completed;
}
void UiRenderer::WaitIdle()
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
	while (CompletedFrames() < SubmittedFrames())
	{
		if (std::chrono::steady_clock::now() > deadline)
			throw std::runtime_error("UI queue completion remains unknown after five seconds");
		SDL_Delay(1);
	}
	Check(vkQueueWaitIdle(m_impl->queue));
}
void UiRenderer::RequestScreenshot(std::string path)
{
	if (!m_impl->screenshots || path.empty())
		throw std::invalid_argument("Screenshot was not negotiated at renderer creation");
	m_impl->screenshotPath = std::move(path);
}
std::string UiRenderer::DeviceName() const
{
	return m_impl->adapter;
}
std::uint64_t UiRenderer::SubmittedFrames() const
{
	return m_impl->submitted;
}
std::uint32_t UiRenderer::RebuildCount() const
{
	return m_impl->creations;
}
} // namespace simxr::app
