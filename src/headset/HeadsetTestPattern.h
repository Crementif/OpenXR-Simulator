#pragma once

#include <array>
#include <cstdint>
#include <openxr/openxr.h>
#include <optional>
#include <vector>
#include <vulkan/vulkan.h>

namespace simxr::headset
{
class HeadsetGraphics;

class HeadsetTestPattern
{
public:
	explicit HeadsetTestPattern(HeadsetGraphics& graphics);
	~HeadsetTestPattern();
	HeadsetTestPattern(const HeadsetTestPattern&) = delete;
	HeadsetTestPattern& operator=(const HeadsetTestPattern&) = delete;

	void Open(XrSession session);
	bool Draw();

	XrSwapchain Swapchain() const;
	XrExtent2Di Extent() const;

private:
	struct Image
	{
		std::array<VkImageView, 2> views{};
		std::array<VkFramebuffer, 2> framebuffers{};
		VkCommandBuffer command = VK_NULL_HANDLE;
		VkFence fence = VK_NULL_HANDLE;
	};

	void Record(Image& image);

	HeadsetGraphics& m_graphics;
	XrSwapchain m_swapchain = XR_NULL_HANDLE;
	VkRenderPass m_renderPass = VK_NULL_HANDLE;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	std::vector<Image> m_images;
	std::optional<std::uint32_t> m_acquired;
	bool m_waited = false;
	static constexpr std::uint32_t s_size = 256;
};
} // namespace simxr::headset
