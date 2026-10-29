#pragma once

#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanNativePreservation.h"
#include "headset/HeadsetImages.h"

#include <array>
#include <optional>

namespace simxr::headset
{
class OpenXrRuntime;

struct HeadsetOutputImage
{
	VkImage image = VK_NULL_HANDLE;
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkFence completion = VK_NULL_HANDLE;
	std::array<VkImageView, 2> views{};
	std::array<VkFramebuffer, 2> framebuffers{};
	std::shared_ptr<const HeadsetImageFrame> source;
	std::shared_ptr<VulkanFrame> mapped;
};

// This whole owner is parked if submission completion becomes unknowable.
struct HeadsetOutputResources
{
	VulkanNativePreservation<HeadsetOutputResources, 4>::Reservation preservation{
		"Headset output preservation capacity exhausted"};
	~HeadsetOutputResources();
	VkDevice device = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkRenderPass labelPass = VK_NULL_HANDLE;
	std::vector<HeadsetOutputImage> images;
	std::unique_ptr<VulkanBackend> mapper;
	FramePool mappingSources{3};
};

class HeadsetOutput
{
public:
	explicit HeadsetOutput(HeadsetGraphics& graphics);
	~HeadsetOutput();

	void Open(OpenXrRuntime& runtime, XrSession session);
	bool Close();

	void Poll();
	// physicalFromWorld maps simulator-world image poses into the current base space.
	// A successful call releases an image and returns the exact projection to submit.
	bool Draw(std::shared_ptr<const HeadsetImageFrame> source, XrPosef physicalFromWorld,
			  const std::array<XrFovf, 2>& physicalFovs = {});
	bool DrawScreen(std::shared_ptr<const HeadsetImageFrame> source);

	XrSwapchain Swapchain() const;
	XrExtent2Di Extent() const;
	std::shared_ptr<VulkanDevice> Device() const;
	const std::array<XrCompositionLayerProjectionView, 2>& Projection() const;

private:
	bool Copy(std::shared_ptr<const HeadsetImageFrame> source, const std::optional<std::array<XrFovf, 2>>& fovs);
	void Record(HeadsetOutputImage& target, const VulkanImage& pixels, bool inspection);
	void Label(HeadsetOutputImage& target);

	HeadsetGraphics& m_graphics;
	std::unique_ptr<HeadsetOutputResources> m_resources;
	XrSwapchain m_swapchain = XR_NULL_HANDLE;
	VkFormat m_format = VK_FORMAT_UNDEFINED;
	XrExtent2Di m_extent{};
	std::optional<std::uint32_t> m_acquired;
	bool m_waited = false;
	std::array<XrCompositionLayerProjectionView, 2> m_projection{};
};
} // namespace simxr::headset
