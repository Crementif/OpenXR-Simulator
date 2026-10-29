#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetGraphics.h"
#include "headset/HeadsetOutput.h"
#include "headset/OpenXrRuntime.h"
#include "math/Math.h"

#define XR_USE_GRAPHICS_API_VULKAN
#include <iostream>
#include <openxr/openxr_platform.h>

namespace simxr::headset::tests
{
struct State
{
	bool mutableFov = true, waited = false, timeout = false, pending = false, failDrain = false;
	unsigned acquired = 0, released = 0, submits = 0, destroyed = 0, blits = 0, labels = 0;
};

static State s_state;

template<class T>
static T Handle(std::uintptr_t id)
{
	return reinterpret_cast<T>(id);
}
} // namespace simxr::headset::tests

namespace simxr
{
struct VulkanBackend::State
{
};

VulkanBackend::VulkanBackend(const VulkanBinding&, std::size_t, std::span<const char* const>, std::uint32_t,
							 std::size_t)
	: m_state(std::make_unique<State>())
{}

VulkanBackend::~VulkanBackend() = default;

XrResult VulkanBackend::Close()
{
	return XR_SUCCESS;
}

void VulkanBackend::RetireCompleted() {}

std::shared_ptr<VulkanDevice> VulkanBackend::Device() const
{
	return {};
}

XrResult VulkanBackend::PrepareRetained(const CompositionPlan&, std::span<const VulkanRetainedSource>,
										std::shared_ptr<VulkanFrame>&)
{
	return XR_ERROR_LIMIT_REACHED;
}

XrResult VulkanBackend::Submit(const std::shared_ptr<VulkanFrame>&)
{
	return XR_SUCCESS;
}

const VulkanImage& VulkanFrame::Image() const
{
	throw std::runtime_error("Unexpected mapped mock image");
}

VulkanImage::VulkanImage(std::shared_ptr<VulkanDevice> owner) : device(std::move(owner)) {}

VulkanImage::~VulkanImage() = default;

std::shared_ptr<VulkanImage> VulkanImage::Create(std::shared_ptr<VulkanDevice> device, VkFormat format,
												 VkExtent2D extent, std::uint32_t layers, VkImageUsageFlags usage,
												 std::uint32_t, VkSampleCountFlagBits, VkDeviceSize, VkImageCreateFlags)
{
	auto result = std::shared_ptr<VulkanImage>(new VulkanImage(std::move(device)));
	result->format = format;
	result->extent = extent;
	result->layers = layers;
	result->usage = usage;
	result->image = headset::tests::Handle<VkImage>(20);
	return result;
}

VulkanError::VulkanError(VkResult result, const char* operation) : std::runtime_error(operation), m_result(result) {}
} // namespace simxr

namespace simxr::control
{
struct VulkanImageConsumer::State
{
	std::shared_ptr<VulkanImage> image;
};

VulkanImageConsumer::VulkanImageConsumer(std::shared_ptr<VulkanDevice>, std::span<const char* const>, std::uint32_t,
										 ImageCompletionKind, bool, bool)
	: m_state(std::make_unique<State>())
{
	m_state->image = VulkanImage::Create({}, VK_FORMAT_R8G8B8A8_SRGB, {800, 600}, 2, VK_IMAGE_USAGE_TRANSFER_SRC_BIT);
}

VulkanImageConsumer::~VulkanImageConsumer() = default;

const VulkanImage& VulkanImageConsumer::Image() const
{
	return *m_state->image;
}
} // namespace simxr::control

namespace simxr::headset
{
HeadsetGraphics::~HeadsetGraphics() = default;

VkInstance HeadsetGraphics::Instance() const
{
	return tests::Handle<VkInstance>(11);
}

VkDevice HeadsetGraphics::Device() const
{
	return tests::Handle<VkDevice>(1);
}

VkPhysicalDevice HeadsetGraphics::PhysicalDevice() const
{
	return tests::Handle<VkPhysicalDevice>(2);
}

std::uint32_t HeadsetGraphics::QueueFamily() const
{
	return 0;
}

bool HeadsetGraphics::Drain()
{
	if (tests::s_state.failDrain)
		PreserveNativeResources();
	return !m_preserved;
}

void HeadsetGraphics::PreserveNativeResources()
{
	m_preserved = true;
}

bool HeadsetGraphics::ResourcesPreserved() const
{
	return m_preserved;
}

OpenXrRuntime::~OpenXrRuntime() = default;

XrInstance OpenXrRuntime::Instance() const
{
	return tests::Handle<XrInstance>(3);
}

XrSystemId OpenXrRuntime::System() const
{
	return 1;
}

const std::vector<XrViewConfigurationView>& OpenXrRuntime::Views() const
{
	static const std::vector<XrViewConfigurationView> views(
		2, {XR_TYPE_VIEW_CONFIGURATION_VIEW, nullptr, 1000, 2000, 900, 2000, 1, 1});
	return views;
}

void CheckVk(VkResult result, const char* operation)
{
	Require(result == VK_SUCCESS, operation);
}

void CheckXr(XrResult result, const char* operation)
{
	Require(XR_SUCCEEDED(result), operation);
}
} // namespace simxr::headset

using namespace simxr::headset::tests;
extern "C"
{
	XRAPI_ATTR XrResult XRAPI_CALL xrGetViewConfigurationProperties(XrInstance, XrSystemId, XrViewConfigurationType,
																	XrViewConfigurationProperties* properties)
	{
		properties->fovMutable = s_state.mutableFov;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainFormats(XrSession, std::uint32_t capacity, std::uint32_t* count,
															   std::int64_t* formats)
	{
		*count = 1;
		if (capacity)
			formats[0] = VK_FORMAT_R8G8B8A8_SRGB;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrCreateSwapchain(XrSession, const XrSwapchainCreateInfo* create,
													 XrSwapchain* swapchain)
	{
		Require(create->width == 1000 && create->height == 900 && create->arraySize == 2 &&
					(create->usageFlags & XR_SWAPCHAIN_USAGE_TRANSFER_DST_BIT),
				"Incorrect output allocation");
		*swapchain = Handle<XrSwapchain>(4);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrEnumerateSwapchainImages(XrSwapchain, std::uint32_t capacity, std::uint32_t* count,
															  XrSwapchainImageBaseHeader* images)
	{
		*count = 1;
		if (capacity)
			reinterpret_cast<XrSwapchainImageVulkan2KHR*>(images)->image = Handle<VkImage>(5);
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrDestroySwapchain(XrSwapchain)
	{
		++s_state.destroyed;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrAcquireSwapchainImage(XrSwapchain, const XrSwapchainImageAcquireInfo*,
														   std::uint32_t* index)
	{
		*index = 0;
		++s_state.acquired;
		s_state.waited = false;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrWaitSwapchainImage(XrSwapchain, const XrSwapchainImageWaitInfo* wait)
	{
		Require(wait->timeout == 0, "Output blocked the frame on a swapchain wait");
		if (s_state.timeout)
			return XR_TIMEOUT_EXPIRED;
		s_state.waited = true;
		return XR_SUCCESS;
	}

	XRAPI_ATTR XrResult XRAPI_CALL xrReleaseSwapchainImage(XrSwapchain, const XrSwapchainImageReleaseInfo*)
	{
		Require(s_state.waited, "Released an unwaited image");
		++s_state.released;
		return XR_SUCCESS;
	}

	VKAPI_ATTR void VKAPI_CALL vkGetPhysicalDeviceFormatProperties(VkPhysicalDevice, VkFormat,
																   VkFormatProperties* properties)
	{
		properties->optimalTilingFeatures = VK_FORMAT_FEATURE_BLIT_SRC_BIT | VK_FORMAT_FEATURE_BLIT_DST_BIT |
											VK_FORMAT_FEATURE_SAMPLED_IMAGE_FILTER_LINEAR_BIT |
											VK_FORMAT_FEATURE_COLOR_ATTACHMENT_BIT;
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkCreateRenderPass(VkDevice, const VkRenderPassCreateInfo*,
													  const VkAllocationCallbacks*, VkRenderPass* pass)
	{
		*pass = Handle<VkRenderPass>(12);
		return VK_SUCCESS;
	}

	VKAPI_ATTR void VKAPI_CALL vkDestroyRenderPass(VkDevice, VkRenderPass, const VkAllocationCallbacks*) {}

	VKAPI_ATTR VkResult VKAPI_CALL vkCreateImageView(VkDevice, const VkImageViewCreateInfo*,
													 const VkAllocationCallbacks*, VkImageView* view)
	{
		*view = Handle<VkImageView>(13);
		return VK_SUCCESS;
	}

	VKAPI_ATTR void VKAPI_CALL vkDestroyImageView(VkDevice, VkImageView, const VkAllocationCallbacks*) {}

	VKAPI_ATTR VkResult VKAPI_CALL vkCreateFramebuffer(VkDevice, const VkFramebufferCreateInfo*,
													   const VkAllocationCallbacks*, VkFramebuffer* framebuffer)
	{
		*framebuffer = Handle<VkFramebuffer>(14);
		return VK_SUCCESS;
	}

	VKAPI_ATTR void VKAPI_CALL vkDestroyFramebuffer(VkDevice, VkFramebuffer, const VkAllocationCallbacks*) {}

	VKAPI_ATTR void VKAPI_CALL vkCmdBeginRenderPass(VkCommandBuffer, const VkRenderPassBeginInfo*, VkSubpassContents)
	{
		++s_state.labels;
	}

	VKAPI_ATTR void VKAPI_CALL vkCmdEndRenderPass(VkCommandBuffer) {}

	VKAPI_ATTR void VKAPI_CALL vkCmdClearAttachments(VkCommandBuffer, std::uint32_t, const VkClearAttachment*,
													 std::uint32_t, const VkClearRect* rectangle)
	{
		Require(rectangle->rect.offset.x >= 0 && rectangle->rect.offset.y >= 0 &&
					rectangle->rect.offset.x + rectangle->rect.extent.width <= 1000 &&
					rectangle->rect.offset.y + rectangle->rect.extent.height <= 900,
				"Inspection lettering escaped its image");
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkCreateCommandPool(VkDevice, const VkCommandPoolCreateInfo*,
													   const VkAllocationCallbacks*, VkCommandPool* pool)
	{
		*pool = Handle<VkCommandPool>(6);
		return VK_SUCCESS;
	}

	VKAPI_ATTR void VKAPI_CALL vkDestroyCommandPool(VkDevice, VkCommandPool, const VkAllocationCallbacks*) {}

	VKAPI_ATTR VkResult VKAPI_CALL vkAllocateCommandBuffers(VkDevice, const VkCommandBufferAllocateInfo*,
															VkCommandBuffer* buffer)
	{
		*buffer = Handle<VkCommandBuffer>(7);
		return VK_SUCCESS;
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkCreateFence(VkDevice, const VkFenceCreateInfo*, const VkAllocationCallbacks*,
												 VkFence* fence)
	{
		*fence = Handle<VkFence>(8);
		return VK_SUCCESS;
	}

	VKAPI_ATTR void VKAPI_CALL vkDestroyFence(VkDevice, VkFence, const VkAllocationCallbacks*) {}

	VKAPI_ATTR VkResult VKAPI_CALL vkGetFenceStatus(VkDevice, VkFence)
	{
		return s_state.pending ? VK_NOT_READY : VK_SUCCESS;
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkResetFences(VkDevice, std::uint32_t, const VkFence*)
	{
		return VK_SUCCESS;
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkResetCommandBuffer(VkCommandBuffer, VkCommandBufferResetFlags)
	{
		return VK_SUCCESS;
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkBeginCommandBuffer(VkCommandBuffer, const VkCommandBufferBeginInfo*)
	{
		return VK_SUCCESS;
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkEndCommandBuffer(VkCommandBuffer)
	{
		return VK_SUCCESS;
	}

	VKAPI_ATTR void VKAPI_CALL vkGetDeviceQueue(VkDevice, std::uint32_t, std::uint32_t, VkQueue* queue)
	{
		*queue = Handle<VkQueue>(9);
	}

	VKAPI_ATTR VkResult VKAPI_CALL vkQueueSubmit(VkQueue, std::uint32_t, const VkSubmitInfo* submit, VkFence)
	{
		Require(submit->waitSemaphoreCount == 0 && submit->commandBufferCount == 1,
				"Repeat waited on one-shot semaphore");
		++s_state.submits;
		s_state.pending = true;
		return VK_SUCCESS;
	}

	VKAPI_ATTR void VKAPI_CALL vkCmdPipelineBarrier(VkCommandBuffer, VkPipelineStageFlags, VkPipelineStageFlags,
													VkDependencyFlags, std::uint32_t, const VkMemoryBarrier*,
													std::uint32_t, const VkBufferMemoryBarrier*, std::uint32_t count,
													const VkImageMemoryBarrier* barriers)
	{
		if (count == 1)
			Require(barriers[0].newLayout == VK_IMAGE_LAYOUT_COLOR_ATTACHMENT_OPTIMAL,
					"Physical image not restored to binding layout");
	}

	VKAPI_ATTR void VKAPI_CALL vkCmdBlitImage(VkCommandBuffer, VkImage, VkImageLayout, VkImage, VkImageLayout,
											  std::uint32_t count, const VkImageBlit* blit, VkFilter filter)
	{
		Require(count == 1 && blit->srcSubresource.layerCount == 2 && blit->dstSubresource.layerCount == 2 &&
					blit->srcOffsets[1].x == 800 && blit->dstOffsets[1].x == 1000 && filter == VK_FILTER_LINEAR,
				"Output lost stereo slices or source/target extent");
		++s_state.blits;
	}
}

int main()
{
	try
	{
		using namespace simxr;
		using namespace simxr::headset;
		HeadsetGraphics graphics;
		OpenXrRuntime runtime;
		HeadsetOutput output(graphics);
		output.Open(runtime, Handle<XrSession>(10));
		auto frame = std::make_shared<HeadsetImageFrame>();
		frame->pixels = std::make_unique<control::VulkanImageConsumer>(nullptr, std::span<const char* const>{}, 0);
		frame->metadata.description.views = {{IdentityPose(), {-.8f, .7f, .9f, -.6f}},
											 {IdentityPose(), {-.7f, .8f, .9f, -.6f}}};
		auto transform = IdentityPose();
		transform.position.x = 2;
		s_state.timeout = true;
		Require(!output.Draw(frame, transform) && s_state.submits == 0, "Unready swapchain was written");
		s_state.timeout = false;
		Require(output.Draw(frame, transform) && s_state.acquired == 1 && s_state.released == 1,
				"Acquire/wait retry lost the physical image");
		Require(output.Projection()[0].pose.position.x == 2 && output.Projection()[0].fov.angleLeft == -.8f &&
					output.Projection()[1].subImage.imageArrayIndex == 1,
				"Output relabelled source pose/FOV/eye");
		s_state.pending = false;
		Require(output.Draw(frame, transform) && s_state.submits == 2 && s_state.released == 2,
				"Repeated private image did not submit independently");
		Require(s_state.labels == 0, "Inspection lettering appeared on immersive output");
		s_state.pending = false;
		Require(output.DrawScreen(frame) && s_state.labels == 2 && output.Extent().width == 1000 &&
					output.Swapchain() != XR_NULL_HANDLE,
				"Stereo inspection output omitted its label");
		std::weak_ptr<const HeadsetImageFrame> retained = frame;
		frame.reset();
		output.Poll();
		Require(!retained.expired(), "Output released source before GPU completion");
		s_state.pending = false;
		output.Poll();
		Require(retained.expired(), "Completed output retained obsolete source");
		Require(output.Close() && s_state.destroyed == 1, "Output did not close cleanly");
		HeadsetOutput unsupported(graphics);
		s_state.mutableFov = false;
		unsupported.Open(runtime, Handle<XrSession>(10));
		Require(unsupported.Close() && s_state.destroyed == 2, "Fixed-FOV output did not open its mapping path");
		bool rejected = false;
		s_state.mutableFov = true;
		auto parked = std::make_shared<HeadsetImageFrame>();
		parked->pixels = std::make_unique<control::VulkanImageConsumer>(nullptr, std::span<const char* const>{}, 0);
		parked->metadata.description.views = {{IdentityPose(), {-.8f, .7f, .9f, -.6f}},
											  {IdentityPose(), {-.7f, .8f, .9f, -.6f}}};
		std::weak_ptr<const HeadsetImageFrame> preserved = parked;
		for (unsigned index = 0; index < 4; ++index)
		{
			HeadsetGraphics failedGraphics;
			HeadsetOutput failed(failedGraphics);
			failed.Open(runtime, Handle<XrSession>(10));
			s_state.pending = false;
			Require(failed.Draw(parked, transform), "Submit preservation fixture");
			s_state.failDrain = true;
			Require(!failed.Close() && failedGraphics.ResourcesPreserved(), "Unknown GPU completion was destroyed");
			s_state.failDrain = false;
		}
		parked.reset();
		Require(!preserved.expired() && s_state.destroyed == 2, "Preservation lost dependent source/swapchain");
		HeadsetGraphics exhaustedGraphics;
		HeadsetOutput exhausted(exhaustedGraphics);
		rejected = false;
		try
		{
			exhausted.Open(runtime, Handle<XrSession>(10));
		}
		catch (const VulkanError&)
		{
			rejected = true;
		}
		Require(rejected, "Preservation capacity was recycled while native resources remained parked");
		std::cout << "PASS headset output ordering and source lifetime; mock Vulkan/OpenXR\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
