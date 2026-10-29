#include "headset/HeadsetTestPattern.h"
#include "Pch.h"
#include "headset/HeadsetGraphics.h"
#include "headset/OpenXrRuntime.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <openxr/openxr_platform.h>

namespace simxr::headset
{
HeadsetTestPattern::HeadsetTestPattern(HeadsetGraphics& graphics) : m_graphics(graphics) {}

HeadsetTestPattern::~HeadsetTestPattern()
{
	if (!m_graphics.Drain())
		return;
	const auto device = m_graphics.Device();
	for (const auto& image : m_images)
	{
		if (image.fence)
			vkDestroyFence(device, image.fence, nullptr);
		for (const auto framebuffer : image.framebuffers)
			if (framebuffer)
				vkDestroyFramebuffer(device, framebuffer, nullptr);
		for (const auto view : image.views)
			if (view)
				vkDestroyImageView(device, view, nullptr);
	}
	if (m_pool)
		vkDestroyCommandPool(device, m_pool, nullptr);
	if (m_renderPass)
		vkDestroyRenderPass(device, m_renderPass, nullptr);
	if (m_swapchain)
		if (XR_FAILED(xrDestroySwapchain(m_swapchain)))
			m_graphics.PreserveNativeResources();
}

void HeadsetTestPattern::Open(XrSession session)
{
	if (m_swapchain)
		throw std::logic_error("Headset test swapchain is already open");
	std::uint32_t count = 0;
	CheckXr(xrEnumerateSwapchainFormats(session, 0, &count, nullptr), "Enumerate physical swapchain formats");
	std::vector<std::int64_t> formats(count);
	CheckXr(xrEnumerateSwapchainFormats(session, count, &count, formats.data()), "Read physical swapchain formats");
	VkFormat format = VK_FORMAT_UNDEFINED;
	for (const auto candidate :
		 {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
	{
		VkFormatProperties properties{};
		vkGetPhysicalDeviceFormatProperties(m_graphics.PhysicalDevice(), candidate, &properties);
		if (std::find(formats.begin(), formats.end(), candidate) != formats.end() &&
			(properties.optimalTilingFeatures & VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT))
		{
			format = candidate;
			break;
		}
	}
	if (format == VK_FORMAT_UNDEFINED)
		throw std::runtime_error("No supported physical RGBA stereo test format");
	XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	create.usageFlags = XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	create.format = format;
	create.sampleCount = 1;
	create.width = create.height = s_size;
	create.faceCount = create.mipCount = 1;
	create.arraySize = 2;
	CheckXr(xrCreateSwapchain(session, &create, &m_swapchain), "Create physical stereo test swapchain");
	CheckXr(xrEnumerateSwapchainImages(m_swapchain, 0, &count, nullptr), "Enumerate physical swapchain images");
	std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
	CheckXr(xrEnumerateSwapchainImages(m_swapchain, count, &count,
									   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
			"Read physical swapchain images");
	if (images.empty())
		throw std::runtime_error("Physical swapchain has no images");
	VkAttachmentDescription attachment{};
	attachment.format = format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_CLEAR;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	VkRenderPassCreateInfo renderPass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	renderPass.attachmentCount = 1;
	renderPass.pAttachments = &attachment;
	renderPass.subpassCount = 1;
	renderPass.pSubpasses = &subpass;
	const auto device = m_graphics.Device();
	CheckVk(vkCreateRenderPass(device, &renderPass, nullptr, &m_renderPass), "Create headset test render pass");
	VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = m_graphics.QueueFamily();
	CheckVk(vkCreateCommandPool(device, &pool, nullptr, &m_pool), "Create headset test command pool");
	m_images.resize(images.size());
	for (std::size_t index = 0; index < images.size(); ++index)
	{
		auto& target = m_images[index];
		VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		allocation.commandPool = m_pool;
		allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocation.commandBufferCount = 1;
		CheckVk(vkAllocateCommandBuffers(device, &allocation, &target.command), "Allocate headset test command");
		VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		CheckVk(vkCreateFence(device, &fence, nullptr, &target.fence), "Create headset test fence");
		for (std::uint32_t eye = 0; eye < 2; ++eye)
		{
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = images[index].image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = format;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
			CheckVk(vkCreateImageView(device, &view, nullptr, &target.views[eye]), "Create headset test eye view");
			VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			framebuffer.renderPass = m_renderPass;
			framebuffer.attachmentCount = 1;
			framebuffer.pAttachments = &target.views[eye];
			framebuffer.width = framebuffer.height = s_size;
			framebuffer.layers = 1;
			CheckVk(vkCreateFramebuffer(device, &framebuffer, nullptr, &target.framebuffers[eye]),
					"Create headset test framebuffer");
		}
	}
}

void HeadsetTestPattern::Record(Image& image)
{
	CheckVk(vkResetCommandBuffer(image.command, 0), "Reset headset test command");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CheckVk(vkBeginCommandBuffer(image.command, &begin), "Begin headset test command");
	// Diagnostic L/R glyphs use attachment clears, not a second compositor/shader set.
	constexpr std::array<std::array<unsigned, 7>, 2> glyphs{
		{{{16, 16, 16, 16, 16, 16, 31}}, {{30, 17, 17, 30, 20, 18, 17}}}};
	for (std::size_t eye = 0; eye < 2; ++eye)
	{
		VkClearValue background{};
		background.color.float32[eye == 0 ? 0 : 2] = 0.3f;
		background.color.float32[3] = 1;
		VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		pass.renderPass = m_renderPass;
		pass.framebuffer = image.framebuffers[eye];
		pass.renderArea.extent = {s_size, s_size};
		pass.clearValueCount = 1;
		pass.pClearValues = &background;
		vkCmdBeginRenderPass(image.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
		VkClearAttachment white{};
		white.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		white.clearValue.color = {{1, 1, 1, 1}};
		for (unsigned row = 0; row < 7; ++row)
			for (unsigned column = 0; column < 5; ++column)
				if (glyphs[eye][row] & (1u << (4 - column)))
				{
					VkClearRect rectangle{
						{{static_cast<int>(48 + column * 32), static_cast<int>(16 + row * 32)}, {32, 32}}, 0, 1};
					vkCmdClearAttachments(image.command, 1, &white, 1, &rectangle);
				}
		vkCmdEndRenderPass(image.command);
	}
	CheckVk(vkEndCommandBuffer(image.command), "End headset test command");
}

bool HeadsetTestPattern::Draw()
{
	if (!m_acquired)
	{
		std::uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		CheckXr(xrAcquireSwapchainImage(m_swapchain, &acquire, &index), "Acquire physical image");
		if (index >= m_images.size())
			throw std::runtime_error("Physical runtime returned an invalid swapchain image index");
		m_acquired = index;
	}
	if (!m_waited)
	{
		XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		wait.timeout = 0;
		const auto result = xrWaitSwapchainImage(m_swapchain, &wait);
		if (result == XR_TIMEOUT_EXPIRED)
			return false;
		CheckXr(result, "Wait for physical image");
		m_waited = true;
	}
	auto& image = m_images[*m_acquired];
	const auto ready = vkGetFenceStatus(m_graphics.Device(), image.fence);
	if (ready == VK_NOT_READY)
		return false;
	CheckVk(ready, "Retire headset test command");
	Record(image);
	CheckVk(vkResetFences(m_graphics.Device(), 1, &image.fence), "Reset headset test fence");
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &image.command;
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(m_graphics.Device(), m_graphics.QueueFamily(), 0, &queue);
	CheckVk(vkQueueSubmit(queue, 1, &submit, image.fence), "Submit headset test image");
	XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	CheckXr(xrReleaseSwapchainImage(m_swapchain, &release), "Release physical image");
	m_acquired.reset();
	m_waited = false;
	return true;
}

XrSwapchain HeadsetTestPattern::Swapchain() const
{
	return m_swapchain;
}

XrExtent2Di HeadsetTestPattern::Extent() const
{
	return {s_size, s_size};
}
} // namespace simxr::headset
