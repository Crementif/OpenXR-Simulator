#pragma once

#include "app/render/UiRenderer.h"
#include <array>
#include <vector>

struct ImGuiContext;

namespace simxr::app
{
// Actual GPU/ImGui owners survive the renderer when a native reader cannot retire.
// Rendering and frame admission remain UiRenderer responsibilities.
struct UiRenderer::Resources
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
	ImGuiContext* context = nullptr;
	VkBuffer readback = VK_NULL_HANDLE;
	VkDeviceMemory readbackMemory = VK_NULL_HANDLE;
	~Resources();
	void DestroySwapchain();
};
} // namespace simxr::app
