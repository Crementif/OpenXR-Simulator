#include "headset/HeadsetOutput.h"
#include "Pch.h"
#include "headset/HeadsetGraphics.h"
#include "headset/HeadsetProjection.h"
#include "headset/OpenXrRuntime.h"
#include "math/Math.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <cmath>
#include <numbers>
#include <openxr/openxr_platform.h>

namespace simxr::headset
{
HeadsetOutputResources::~HeadsetOutputResources()
{
	for (const auto& image : images)
	{
		if (image.completion)
			vkDestroyFence(device, image.completion, nullptr);
		for (auto framebuffer : image.framebuffers)
			if (framebuffer)
				vkDestroyFramebuffer(device, framebuffer, nullptr);
		for (auto view : image.views)
			if (view)
				vkDestroyImageView(device, view, nullptr);
	}
	if (labelPass)
		vkDestroyRenderPass(device, labelPass, nullptr);
	if (pool)
		vkDestroyCommandPool(device, pool, nullptr);
}
HeadsetOutput::HeadsetOutput(HeadsetGraphics& graphics) : m_graphics(graphics) {}
HeadsetOutput::~HeadsetOutput()
{
	Close();
}
bool HeadsetOutput::Close()
{
	if (!m_resources && !m_swapchain)
		return !m_graphics.ResourcesPreserved();
	if (!m_graphics.Drain())
	{
		if (m_resources)
			m_resources->preservation.Preserve(std::move(m_resources));
		return false;
	}
	if (m_resources && m_resources->mapper)
	{
		for (auto& image : m_resources->images)
		{
			image.source.reset();
			image.mapped.reset();
		}
		if (XR_FAILED(m_resources->mapper->Close()))
		{
			m_graphics.PreserveNativeResources();
			m_resources->preservation.Preserve(std::move(m_resources));
			return false;
		}
	}
	m_resources.reset();
	if (m_swapchain)
	{
		if (XR_FAILED(xrDestroySwapchain(m_swapchain)))
		{
			m_graphics.PreserveNativeResources();
			return false;
		}
		m_swapchain = XR_NULL_HANDLE;
	}
	m_acquired.reset();
	m_waited = false;
	return true;
}
void HeadsetOutput::Open(OpenXrRuntime& runtime, XrSession session)
{
	if (m_resources || m_swapchain || m_graphics.ResourcesPreserved())
		throw std::logic_error("Headset output is already open or preserved");
	XrViewConfigurationProperties properties{XR_TYPE_VIEW_CONFIGURATION_PROPERTIES};
	CheckXr(xrGetViewConfigurationProperties(runtime.Instance(), runtime.System(),
											 XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, &properties),
			"Read physical projection properties");

	const auto& views = runtime.Views();
	if (views.size() != 2)
		throw std::runtime_error("Headset output requires stereo view recommendations");
	std::uint32_t width = 0, height = 0;
	for (const auto& view : views)
	{
		width = std::max(width, view.recommendedImageRectWidth);
		height = std::max(height, view.recommendedImageRectHeight);
	}
	for (const auto& view : views)
		if (!width || !height || width > view.maxImageRectWidth || height > view.maxImageRectHeight ||
			width > INT32_MAX || height > INT32_MAX)
			throw std::runtime_error("Physical stereo recommendations exceed supported image extents");
	m_extent = {static_cast<std::int32_t>(width), static_cast<std::int32_t>(height)};
	std::uint32_t count = 0;
	CheckXr(xrEnumerateSwapchainFormats(session, 0, &count, nullptr), "Enumerate headset output formats");
	std::vector<std::int64_t> formats(count);
	CheckXr(xrEnumerateSwapchainFormats(session, count, &count, formats.data()), "Read headset output formats");
	m_format = VK_FORMAT_UNDEFINED;
	for (const auto candidate :
		 {VK_FORMAT_R8G8B8A8_SRGB, VK_FORMAT_B8G8R8A8_SRGB, VK_FORMAT_R8G8B8A8_UNORM, VK_FORMAT_B8G8R8A8_UNORM})
	{
		VkFormatProperties format{};
		vkGetPhysicalDeviceFormatProperties(m_graphics.PhysicalDevice(), candidate, &format);
		if (std::find(formats.begin(), formats.end(), candidate) != formats.end() &&
			((format.optimalTilingFeatures &
			  (VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)) ==
			 (VK_FORMAT_FEATURE_BLIT_DST_BIT | VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT)))
		{
			m_format = candidate;
			break;
		}
	}
	if (m_format == VK_FORMAT_UNDEFINED)
		throw std::runtime_error("Physical runtime has no supported RGBA transfer destination");
	m_resources = std::make_unique<HeadsetOutputResources>();
	m_resources->device = m_graphics.Device();
	if (!properties.fovMutable)
		m_resources->mapper =
			std::make_unique<VulkanBackend>(VulkanBinding{m_graphics.Instance(), m_graphics.PhysicalDevice(),
														  m_graphics.Device(), m_graphics.QueueFamily(), 0},
											3);
	XrSwapchainCreateInfo create{XR_TYPE_SWAPCHAIN_CREATE_INFO};
	create.usageFlags = XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT | XR_SWAPCHAIN_USAGE_COLOR_ATTACHMENT_BIT;
	create.format = m_format;
	create.width = width;
	create.height = height;
	create.arraySize = 2;
	create.sampleCount = create.faceCount = create.mipCount = 1;
	CheckXr(xrCreateSwapchain(session, &create, &m_swapchain), "Create physical output swapchain");
	CheckXr(xrEnumerateSwapchainImages(m_swapchain, 0, &count, nullptr), "Enumerate physical output images");
	std::vector<XrSwapchainImageVulkan2KHR> images(count, {XR_TYPE_SWAPCHAIN_IMAGE_VULKAN2_KHR});
	CheckXr(xrEnumerateSwapchainImages(m_swapchain, count, &count,
									   reinterpret_cast<XrSwapchainImageBaseHeader*>(images.data())),
			"Read physical output images");
	if (!count || count > images.size())
		throw std::runtime_error("Physical output returned an invalid image count");
	images.resize(count);
	VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	pool.queueFamilyIndex = m_graphics.QueueFamily();
	CheckVk(vkCreateCommandPool(m_resources->device, &pool, nullptr, &m_resources->pool), "Create output command pool");
	VkAttachmentDescription attachment{};
	attachment.format = m_format;
	attachment.samples = VK_SAMPLE_COUNT_1_BIT;
	attachment.loadOp = VK_ATTACHMENT_LOAD_OP_LOAD;
	attachment.storeOp = VK_ATTACHMENT_STORE_OP_STORE;
	attachment.stencilLoadOp = VK_ATTACHMENT_LOAD_OP_DONT_CARE;
	attachment.stencilStoreOp = VK_ATTACHMENT_STORE_OP_DONT_CARE;
	attachment.initialLayout = attachment.finalLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	VkAttachmentReference reference{0, VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL};
	VkSubpassDescription subpass{};
	subpass.pipelineBindPoint = VK_PIPELINE_BIND_POINT_GRAPHICS;
	subpass.colorAttachmentCount = 1;
	subpass.pColorAttachments = &reference;
	VkRenderPassCreateInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_CREATE_INFO};
	pass.attachmentCount = 1;
	pass.pAttachments = &attachment;
	pass.subpassCount = 1;
	pass.pSubpasses = &subpass;
	CheckVk(vkCreateRenderPass(m_resources->device, &pass, nullptr, &m_resources->labelPass),
			"Create inspection label pass");
	m_resources->images.resize(count);
	for (std::size_t index = 0; index < count; ++index)
	{
		auto& output = m_resources->images[index];
		output.image = images[index].image;
		for (std::uint32_t eye = 0; eye < 2; ++eye)
		{
			VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
			view.image = output.image;
			view.viewType = VK_IMAGE_VIEW_TYPE_2D;
			view.format = m_format;
			view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, eye, 1};
			CheckVk(vkCreateImageView(m_resources->device, &view, nullptr, &output.views[eye]),
					"Create inspection eye view");
			VkFramebufferCreateInfo framebuffer{VK_STRUCTURE_TYPE_FRAMEBUFFER_CREATE_INFO};
			framebuffer.renderPass = m_resources->labelPass;
			framebuffer.attachmentCount = 1;
			framebuffer.pAttachments = &output.views[eye];
			framebuffer.width = width;
			framebuffer.height = height;
			framebuffer.layers = 1;
			CheckVk(vkCreateFramebuffer(m_resources->device, &framebuffer, nullptr, &output.framebuffers[eye]),
					"Create inspection framebuffer");
		}
		VkCommandBufferAllocateInfo allocation{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		allocation.commandPool = m_resources->pool;
		allocation.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		allocation.commandBufferCount = 1;
		CheckVk(vkAllocateCommandBuffers(m_resources->device, &allocation, &output.command), "Allocate output command");
		VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		fence.flags = VK_FENCE_CREATE_SIGNALED_BIT;
		CheckVk(vkCreateFence(m_resources->device, &fence, nullptr, &output.completion),
				"Create output completion fence");
	}
}
void HeadsetOutput::Poll()
{
	if (!m_resources)
		return;
	for (auto& image : m_resources->images)
		if (image.source)
		{
			const auto status = vkGetFenceStatus(m_resources->device, image.completion);
			if (status == VK_NOT_READY)
				continue;
			CheckVk(status, "Retire physical output source");
			image.source.reset();
			image.mapped.reset();
		}
	if (m_resources->mapper)
		m_resources->mapper->RetireCompleted();
}
void HeadsetOutput::Label(HeadsetOutputImage& target)
{
	// Small fixed diagnostic lettering, rendered with attachment clears like the test pattern.
	constexpr std::array<std::array<unsigned, 7>, 10> letters{{{31, 4, 4, 4, 4, 4, 31},
															   {17, 25, 25, 21, 19, 19, 17},
															   {15, 16, 16, 14, 1, 1, 30},
															   {30, 17, 17, 30, 16, 16, 16},
															   {31, 16, 16, 30, 16, 16, 31},
															   {15, 16, 16, 16, 16, 16, 15},
															   {31, 4, 4, 4, 4, 4, 4},
															   {31, 4, 4, 4, 4, 4, 31},
															   {14, 17, 17, 17, 17, 17, 14},
															   {17, 25, 25, 21, 19, 19, 17}}};
	const auto scale = std::min(m_extent.width / 64, m_extent.height / 10);
	if (scale < 1)
		throw std::runtime_error("Physical image is too small for the inspection label");
	for (std::size_t eye = 0; eye < 2; ++eye)
	{
		VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
		pass.renderPass = m_resources->labelPass;
		pass.framebuffer = target.framebuffers[eye];
		pass.renderArea.extent = {static_cast<std::uint32_t>(m_extent.width),
								  static_cast<std::uint32_t>(m_extent.height)};
		vkCmdBeginRenderPass(target.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
		VkClearAttachment ink{};
		ink.aspectMask = VK_IMAGE_ASPECT_COLOR_BIT;
		ink.clearValue.color = {{.02f, .02f, .02f, 1}};
		VkClearRect band{
			{{0, 0}, {static_cast<std::uint32_t>(m_extent.width), static_cast<std::uint32_t>(scale * 9)}}, 0, 1};
		vkCmdClearAttachments(target.command, 1, &ink, 1, &band);
		ink.clearValue.color = {{1, 1, 1, 1}};
		for (std::size_t letter = 0; letter < letters.size(); ++letter)
			for (unsigned row = 0; row < 7; ++row)
				for (unsigned column = 0; column < 5; ++column)
					if (letters[letter][row] & (1u << (4 - column)))
					{
						VkClearRect cell{
							{{static_cast<int>((letter * 6 + column + 2) * scale), static_cast<int>((row + 1) * scale)},
							 {static_cast<std::uint32_t>(scale), static_cast<std::uint32_t>(scale)}},
							0,
							1};
						vkCmdClearAttachments(target.command, 1, &ink, 1, &cell);
					}
		vkCmdEndRenderPass(target.command);
	}
}
void HeadsetOutput::Record(HeadsetOutputImage& target, const VulkanImage& pixels, bool inspection)
{
	VkFormatProperties properties{};
	vkGetPhysicalDeviceFormatProperties(m_graphics.PhysicalDevice(), pixels.format, &properties);
	constexpr auto required = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT;
	if ((properties.optimalTilingFeatures & required) != required || pixels.layers != 2 ||
		!(pixels.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || pixels.extent.width > INT32_MAX ||
		pixels.extent.height > INT32_MAX)
		throw std::runtime_error("Private headset image cannot be scaled by the physical device");
	CheckVk(vkResetCommandBuffer(target.command, 0), "Reset physical output command");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CheckVk(vkBeginCommandBuffer(target.command, &begin), "Begin physical output command");
	std::array<VkImageMemoryBarrier, 2> barriers{};
	for (auto& barrier : barriers)
	{
		barrier.sType = VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 2};
	}
	barriers[0].image = pixels.image;
	barriers[0].oldLayout = barriers[0].newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barriers[0].srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	barriers[0].dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barriers[1].image = target.image;
	barriers[1].oldLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	barriers[1].newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barriers[1].dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(target.command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
						 nullptr, 0, nullptr, barriers.size(), barriers.data());
	VkImageBlit blit{};
	blit.srcSubresource = blit.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 2};
	blit.srcOffsets[1] = {static_cast<std::int32_t>(pixels.extent.width),
						  static_cast<std::int32_t>(pixels.extent.height), 1};
	blit.dstOffsets[1] = {m_extent.width, m_extent.height, 1};
	vkCmdBlitImage(target.command, pixels.image, VK_IMAGE_LAYOUT_GENERAL, target.image,
				   VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL, 1, &blit, VK_FILTER_LINEAR);
	auto release = barriers[1];
	release.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	release.newLayout = VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL;
	release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	release.dstAccessMask = VK_ACCESS_COLOR_ATTACHMENT_READ_BIT | VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	vkCmdPipelineBarrier(target.command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT,
						 0, 0, nullptr, 0, nullptr, 1, &release);
	if (inspection)
		Label(target);
	CheckVk(vkEndCommandBuffer(target.command), "Finish physical output command");
}
bool HeadsetOutput::Draw(std::shared_ptr<const HeadsetImageFrame> source, XrPosef physicalFromWorld,
						 const std::array<XrFovf, 2>& physicalFovs)
{
	if (!m_resources || !m_swapchain)
		throw std::logic_error("Physical output is not open");
	Poll();
	if (!source)
		return false;
	const auto transform = NormalizedPose(physicalFromWorld);
	if (!transform || source->metadata.description.views.size() != 2)
		throw std::invalid_argument("Physical output requires calibrated stereo projection metadata");
	auto projection = m_projection;
	for (std::size_t eye = 0; eye < projection.size(); ++eye)
	{
		const auto& view = source->metadata.description.views[eye];
		const auto pose = NormalizedPose(view.pose);
		if (!pose)
			throw std::invalid_argument("Physical output source has an invalid eye pose");
		projection[eye] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW};
		const auto mapped = NormalizedPose(Compose(*transform, *pose));
		if (!mapped)
			throw std::invalid_argument("Mapped physical eye pose is not finite");
		for (const auto angle : {view.fov.angleLeft, view.fov.angleRight, view.fov.angleUp, view.fov.angleDown})
			if (!std::isfinite(angle) || std::abs(angle) >= std::numbers::pi_v<float> / 2)
				throw std::invalid_argument("Physical output source has an invalid FOV");
		if (view.fov.angleLeft >= view.fov.angleRight || view.fov.angleDown >= view.fov.angleUp)
			throw std::invalid_argument("Physical output source has an empty FOV");
		projection[eye].pose = *mapped;
		projection[eye].fov = m_resources->mapper ? physicalFovs[eye] : view.fov;
		projection[eye].subImage = {m_swapchain, {{0, 0}, m_extent}, static_cast<std::uint32_t>(eye)};
	}
	if (!Copy(std::move(source), physicalFovs))
		return false;
	m_projection = projection;
	return true;
}
bool HeadsetOutput::DrawScreen(std::shared_ptr<const HeadsetImageFrame> source)
{
	return Copy(std::move(source), std::nullopt);
}
XrSwapchain HeadsetOutput::Swapchain() const
{
	return m_swapchain;
}
XrExtent2Di HeadsetOutput::Extent() const
{
	return m_extent;
}
bool HeadsetOutput::Copy(std::shared_ptr<const HeadsetImageFrame> source,
						 const std::optional<std::array<XrFovf, 2>>& fovs)
{
	if (!m_resources || !m_swapchain)
		throw std::logic_error("Physical output is not open");
	Poll();
	if (!source)
		return false;
	if (!m_acquired)
	{
		std::uint32_t index = 0;
		XrSwapchainImageAcquireInfo acquire{XR_TYPE_SWAPCHAIN_IMAGE_ACQUIRE_INFO};
		CheckXr(xrAcquireSwapchainImage(m_swapchain, &acquire, &index), "Acquire physical output image");
		if (index >= m_resources->images.size())
			throw std::runtime_error("Invalid physical output image index");
		m_acquired = index;
	}
	if (!m_waited)
	{
		XrSwapchainImageWaitInfo wait{XR_TYPE_SWAPCHAIN_IMAGE_WAIT_INFO};
		const auto result = xrWaitSwapchainImage(m_swapchain, &wait);
		if (result == XR_TIMEOUT_EXPIRED)
			return false;
		CheckXr(result, "Wait for physical output image");
		m_waited = true;
	}
	auto& image = m_resources->images[*m_acquired];
	const auto status = vkGetFenceStatus(m_resources->device, image.completion);
	if (status == VK_NOT_READY)
		return false;
	CheckVk(status, "Retire physical output command");
	const auto& input = source->pixels->Image();
	if (m_resources->mapper && fovs)
	{
		CompositionPlan plan;
		CheckXr(PrepareHeadsetProjection(
					source->metadata.description.views,
					{static_cast<std::int32_t>(input.extent.width), static_cast<std::int32_t>(input.extent.height)},
					m_extent, *fovs, plan),
				"Prepare physical FOV mapping");
		auto budget = m_resources->mappingSources.Reserve();
		if (!budget || !m_resources->mappingSources.Consume(budget))
			return false;
		auto retained = std::shared_ptr<VulkanImage>(source, const_cast<VulkanImage*>(&input));
		const std::array<VulkanRetainedSource, 2> sources{{{retained, budget}, {retained, budget}}};
		const auto prepared = m_resources->mapper->PrepareRetained(plan, sources, image.mapped);
		if (prepared == XR_ERROR_LIMIT_REACHED)
			return false;
		CheckXr(prepared, "Prepare physical projection image");
		CheckXr(m_resources->mapper->Submit(image.mapped), "Submit physical projection image");
	}
	Record(image, image.mapped ? image.mapped->Image() : input, !fovs);
	CheckVk(vkResetFences(m_resources->device, 1, &image.completion), "Reset physical output completion");
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &image.command;
	VkQueue queue = VK_NULL_HANDLE;
	vkGetDeviceQueue(m_resources->device, m_graphics.QueueFamily(), 0, &queue);
	// Retain before submission, including any result with ambiguous native acceptance.
	image.source = std::move(source);
	CheckVk(vkQueueSubmit(queue, 1, &submit, image.completion), "Submit physical output");
	XrSwapchainImageReleaseInfo release{XR_TYPE_SWAPCHAIN_IMAGE_RELEASE_INFO};
	CheckXr(xrReleaseSwapchainImage(m_swapchain, &release), "Release physical output image");
	m_acquired.reset();
	m_waited = false;
	return true;
}
const std::array<XrCompositionLayerProjectionView, 2>& HeadsetOutput::Projection() const
{
	return m_projection;
}
std::shared_ptr<VulkanDevice> HeadsetOutput::Device() const
{
	return m_resources && m_resources->mapper ? m_resources->mapper->Device() : nullptr;
}
} // namespace simxr::headset
