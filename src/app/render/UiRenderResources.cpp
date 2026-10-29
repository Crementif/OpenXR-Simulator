#include "app/render/UiRenderResources.h"
#include "app/UiPch.h"
#include "app/render/UiRenderer.h"
#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

namespace simxr::app
{
UiRenderer::Resources::~Resources()
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

void UiRenderer::Resources::DestroySwapchain()
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
} // namespace simxr::app
