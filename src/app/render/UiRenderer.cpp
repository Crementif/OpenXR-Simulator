#include "app/render/UiRenderer.h"
#include "app/UiPch.h"
#include "app/platform/Window.h"
#include "app/render/UiRenderResources.h"

#include <ImGuizmo.h>

#include <imgui_impl_sdl3.h>
#include <imgui_impl_vulkan.h>

namespace simxr::app
{
void UiRenderer::CheckResult(VkResult result)
{
	if (result != VK_SUCCESS)
		throw std::runtime_error("Companion Vulkan operation failed: " + std::to_string(result));
}

void UiRenderer::ProcessEvent(const SDL_Event& event)
{
	ImGui::SetCurrentContext(m_resources->context);
	ImGui_ImplSDL3_ProcessEvent(&event);
}

bool UiRenderer::BeginFrame()
{
	auto& gpu = *m_resources;
	if (m_frameStarted)
		throw std::logic_error("The admitted UI frame must be rendered before beginning another");
	ImGui::SetCurrentContext(gpu.context);
	const auto [width, height] = m_window->PixelSize();
	const bool acquired =
		std::any_of(gpu.frames.begin(), gpu.frames.end(), [](const auto& frame) { return frame.hasImage; });
	if (!acquired)
	{
		if (width <= 0 || height <= 0 || (SDL_GetWindowFlags(m_window->Native()) & SDL_WINDOW_MINIMIZED))
			return false;
		if (m_rebuild || gpu.extent.width != static_cast<std::uint32_t>(width) ||
			gpu.extent.height != static_cast<std::uint32_t>(height))
			CreateSwapchain();
	}
	if (!m_rendererReady)
		return false;
	// Keep SDL/ImGui events queued until this frame has a usable image. An
	// unseen UI frame must not consume clicks, popup state or docking edits.
	auto& frame = gpu.frames[m_frameSlot];
	if (!frame.hasImage)
	{
		const auto ready = vkWaitForFences(gpu.device, 1, &frame.fence, VK_TRUE, 0);
		if (ready == VK_TIMEOUT)
			return false;
		CheckResult(ready);
		m_completed = std::max(m_completed, frame.serial);
		const auto acquired =
			vkAcquireNextImageKHR(gpu.device, gpu.swapchain, 0, frame.acquired, VK_NULL_HANDLE, &frame.image);
		if (acquired == VK_NOT_READY || acquired == VK_TIMEOUT)
			return false;
		if (acquired == VK_ERROR_OUT_OF_DATE_KHR)
		{
			m_rebuild = true;
			return false;
		}
		if (acquired == VK_SUBOPTIMAL_KHR)
			m_rebuild = true;
		else
			CheckResult(acquired);
		frame.hasImage = true;
	}
	if (gpu.imageFences[frame.image])
	{
		const auto ready = vkWaitForFences(gpu.device, 1, &gpu.imageFences[frame.image], VK_TRUE, 0);
		if (ready == VK_TIMEOUT)
			return false;
		CheckResult(ready);
	}
	ImGui_ImplVulkan_NewFrame();
	ImGui_ImplSDL3_NewFrame();
	ImGui::NewFrame();
	ImGuizmo::BeginFrame();
	m_frameStarted = true;
	return true;
}

bool UiRenderer::Render(std::span<const VkSemaphore> readyWaits, const std::function<void(VkCommandBuffer)>& transfers)
{
	auto& gpu = *m_resources;
	if (!m_frameStarted)
		throw std::logic_error("UI rendering requires an admitted frame");
	m_frameStarted = false;
	ImGui::Render();
	auto& frame = gpu.frames[m_frameSlot];
	const bool capture = !m_screenshotPath.empty();
	if (capture)
		PrepareReadback();
	CheckResult(vkResetCommandPool(gpu.device, frame.pool, 0));
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CheckResult(vkBeginCommandBuffer(frame.command, &begin));
	if (transfers)
		transfers(frame.command);
	VkClearValue clear{};
	clear.color = {{0.04f, 0.05f, 0.07f, 1}};
	VkRenderPassBeginInfo pass{VK_STRUCTURE_TYPE_RENDER_PASS_BEGIN_INFO};
	pass.renderPass = gpu.renderPass;
	pass.framebuffer = gpu.framebuffers[frame.image];
	pass.renderArea.extent = gpu.extent;
	pass.clearValueCount = 1;
	pass.pClearValues = &clear;
	vkCmdBeginRenderPass(frame.command, &pass, VK_SUBPASS_CONTENTS_INLINE);
	ImGui_ImplVulkan_RenderDrawData(ImGui::GetDrawData(), frame.command);
	vkCmdEndRenderPass(frame.command);
	if (capture)
		RecordReadback(frame.command, gpu.swapchainImages[frame.image]);
	CheckResult(vkEndCommandBuffer(frame.command));
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
	submit.pSignalSemaphores = &gpu.rendered[frame.image];
	CheckResult(vkResetFences(gpu.device, 1, &frame.fence));
	const auto accepted = vkQueueSubmit(gpu.queue, 1, &submit, frame.fence);
	// OOM guarantees rejection. Other failures may leave native work in
	// flight: expose its serial before throwing so panel retirement cannot
	// free descriptors/private images referenced by that submission.
	if (accepted != VK_ERROR_OUT_OF_HOST_MEMORY && accepted != VK_ERROR_OUT_OF_DEVICE_MEMORY)
	{
		frame.serial = ++m_submitted;
		frame.hasImage = false;
	}
	CheckResult(accepted);
	frame.hasImage = false;
	gpu.imageFences[frame.image] = frame.fence;
	VkPresentInfoKHR present{VK_STRUCTURE_TYPE_PRESENT_INFO_KHR};
	present.waitSemaphoreCount = 1;
	present.pWaitSemaphores = &gpu.rendered[frame.image];
	present.swapchainCount = 1;
	present.pSwapchains = &gpu.swapchain;
	present.pImageIndices = &frame.image;
	const auto result = vkQueuePresentKHR(gpu.queue, &present);
	if (result == VK_ERROR_OUT_OF_DATE_KHR || result == VK_SUBOPTIMAL_KHR)
		m_rebuild = true;
	else
		CheckResult(result);
	if (capture)
	{
		CheckResult(vkWaitForFences(gpu.device, 1, &frame.fence, VK_TRUE, 5000000000ULL));
		WriteScreenshot();
	}
	m_frameSlot = (m_frameSlot + 1) % gpu.frames.size();
	return true;
}

UiRenderer::UiRenderer(Window& window, bool validation, std::shared_ptr<ValidationLog> log, bool screenshots,
					   bool proofConsumer)
	: m_resources(std::make_shared<Resources>())
{
	Initialize(window, validation, std::move(log), screenshots, proofConsumer);
}

UiRenderer::~UiRenderer() = default;

VulkanDeviceAccess UiRenderer::DeviceAccess() const
{
	auto& gpu = *m_resources;
	return {gpu.instance,		gpu.physical,	   gpu.device,	gpu.family,			 gpu.ioQueue,
			gpu.externalImages, gpu.externalFence, m_resources, gpu.externalTimeline};
}

std::uint64_t UiRenderer::CompletedFrames()
{
	auto& gpu = *m_resources;
	for (const auto& frame : gpu.frames)
		if (frame.serial > m_completed)
		{
			const auto result = vkGetFenceStatus(gpu.device, frame.fence);
			if (result == VK_SUCCESS)
				m_completed = std::max(m_completed, frame.serial);
			else if (result != VK_NOT_READY)
				CheckResult(result);
		}
	return m_completed;
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
	CheckResult(vkQueueWaitIdle(m_resources->queue));
}

void UiRenderer::RequestScreenshot(std::filesystem::path path)
{
	if (!m_screenshots || path.empty())
		throw std::invalid_argument("Screenshot was not negotiated at renderer creation");
	if (path.native().find(std::filesystem::path::value_type{}) != std::filesystem::path::string_type::npos)
		throw std::invalid_argument("Screenshot path contains an embedded NUL");
	m_screenshotPath = std::move(path);
}

std::string UiRenderer::DeviceName() const
{
	return m_adapter;
}

std::uint64_t UiRenderer::SubmittedFrames() const
{
	return m_submitted;
}

std::uint32_t UiRenderer::RebuildCount() const
{
	return m_creations;
}
} // namespace simxr::app
