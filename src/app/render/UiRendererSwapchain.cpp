#include "CompanionSpirv.h"
#include "app/UiPch.h"
#include "app/platform/Window.h"
#include "app/render/UiRenderResources.h"
#include "app/render/UiRenderer.h"
#include <imgui_impl_vulkan.h>

namespace simxr::app
{
static bool IsSrgb(VkFormat format)
{
	return format == VK_FORMAT_B8G8R8A8_SRGB || format == VK_FORMAT_R8G8B8A8_SRGB;
}

void UiRenderer::CreateSwapchain()
{
	auto& gpu = *m_resources;
	const auto [width, height] = m_window->PixelSize();
	if (width <= 0 || height <= 0)
		return;
	// The IO queue may be waiting on an external producer. Resizing only
	// retires UI work and must not wait for that independent queue.
	CheckResult(vkQueueWaitIdle(gpu.queue));
	m_completed = m_submitted;
	gpu.DestroySwapchain();
	VkSurfaceCapabilitiesKHR capabilities;
	CheckResult(vkGetPhysicalDeviceSurfaceCapabilitiesKHR(gpu.physical, gpu.surface, &capabilities));
	std::uint32_t count = 0;
	CheckResult(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, gpu.surface, &count, nullptr));
	std::vector<VkSurfaceFormatKHR> formats(count);
	CheckResult(vkGetPhysicalDeviceSurfaceFormatsKHR(gpu.physical, gpu.surface, &count, formats.data()));
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
	if (m_rendererReady && IsSrgb(gpu.format) != IsSrgb(selected.format))
		throw std::runtime_error("The display color format changed. Reopen the companion.");
	gpu.format = selected.format;
	gpu.extent = capabilities.currentExtent;
	if (gpu.extent.width == std::numeric_limits<std::uint32_t>::max())
		gpu.extent = {std::clamp(static_cast<std::uint32_t>(width), capabilities.minImageExtent.width,
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
	create.surface = gpu.surface;
	create.minImageCount = imageCount;
	create.imageFormat = gpu.format;
	create.imageColorSpace = selected.colorSpace;
	create.imageExtent = gpu.extent;
	create.imageArrayLayers = 1;
	create.imageUsage = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT;
	if (m_screenshots)
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
	CheckResult(vkCreateSwapchainKHR(gpu.device, &create, nullptr, &gpu.swapchain));
	CheckResult(vkGetSwapchainImagesKHR(gpu.device, gpu.swapchain, &count, nullptr));
	std::vector<VkImage> images(count);
	CheckResult(vkGetSwapchainImagesKHR(gpu.device, gpu.swapchain, &count, images.data()));
	gpu.swapchainImages = images;
	VkAttachmentDescription attachment{};
	attachment.format = gpu.format;
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
	CheckResult(vkCreateRenderPass(gpu.device, &pass, nullptr, &gpu.renderPass));
	gpu.views.resize(count);
	gpu.framebuffers.resize(count);
	gpu.rendered.resize(count);
	gpu.imageFences.assign(count, VK_NULL_HANDLE);
	for (std::uint32_t index = 0; index < count; ++index)
	{
		VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		view.image = images[index];
		view.viewType = VK_IMAGE_VIEW_TYPE_2D;
		view.format = gpu.format;
		view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		CheckResult(vkCreateImageView(gpu.device, &view, nullptr, &gpu.views[index]));
		VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
		framebuffer.renderPass = gpu.renderPass;
		framebuffer.attachmentCount = 1;
		framebuffer.pAttachments = &gpu.views[index];
		framebuffer.width = gpu.extent.width;
		framebuffer.height = gpu.extent.height;
		framebuffer.layers = 1;
		CheckResult(vkCreateFramebuffer(gpu.device, &framebuffer, nullptr, &gpu.framebuffers[index]));
		VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
		CheckResult(vkCreateSemaphore(gpu.device, &semaphore, nullptr, &gpu.rendered[index]));
	}
	ImGui_ImplVulkan_PipelineInfo pipeline{};
	pipeline.RenderPass = gpu.renderPass;
	pipeline.MSAASamples = VK_SAMPLE_COUNT_1_BIT;
	if (m_rendererReady)
		ImGui_ImplVulkan_CreateMainPipeline(&pipeline);
	else
	{
		ImGui_ImplVulkan_InitInfo backend{};
		backend.ApiVersion = VK_API_VERSION_1_1;
		backend.CheckVkResultFn = CheckResult;
		backend.Instance = gpu.instance;
		backend.PhysicalDevice = gpu.physical;
		backend.Device = gpu.device;
		backend.QueueFamily = gpu.family;
		backend.Queue = gpu.queue;
		backend.DescriptorPool = gpu.descriptors;
		// ImGui's upload ring follows our in-flight submissions, not the
		// swapchain image count. Keep both backends alive across resizes:
		// Vulkan backend shutdown also destroys platform viewport handles.
		backend.MinImageCount = static_cast<std::uint32_t>(gpu.frames.size());
		backend.ImageCount = static_cast<std::uint32_t>(gpu.frames.size());
		backend.PipelineInfoMain = pipeline;
		const bool srgb = IsSrgb(gpu.format);
		backend.CustomShaderFragCreateInfo.sType = VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO;
		backend.CustomShaderFragCreateInfo.codeSize = srgb ? sizeof(shader::srgb) : sizeof(shader::unorm);
		backend.CustomShaderFragCreateInfo.pCode = srgb ? shader::srgb.data() : shader::unorm.data();
		if (!ImGui_ImplVulkan_Init(&backend))
			throw std::runtime_error("ImGui Vulkan initialization failed");
		m_rendererReady = true;
	}
	m_rebuild = false;
	++m_creations;
}
} // namespace simxr::app
