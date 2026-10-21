#include "graphics/backends/vulkan/VulkanSubmission.h"

#include <cstdio>
#include <utility>

namespace simxr
{
VulkanSubmission::Resources::Resources(std::shared_ptr<VulkanDevice> owner) : device(std::move(owner)) {}

VulkanSubmission::Resources::~Resources()
{
	const auto native = device->binding.device;
	for (auto framebuffer : framebuffers)
		if (framebuffer)
			vkDestroyFramebuffer(native, framebuffer, nullptr);
	if (pool)
		vkDestroyCommandPool(native, pool, nullptr);
	if (descriptors)
		vkDestroyDescriptorPool(native, descriptors, nullptr);
	for (auto view : sourceViews)
		vkDestroyImageView(native, view, nullptr);
	for (auto view : outputViews)
		if (view)
			vkDestroyImageView(native, view, nullptr);
	if (fence)
		vkDestroyFence(native, fence, nullptr);
}

VulkanSubmission::VulkanSubmission(std::shared_ptr<VulkanDevice> device)
	: m_resources(std::make_unique<Resources>(std::move(device)))
{
	const auto native = m_resources->device->binding.device;
	VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool.queueFamilyIndex = m_resources->device->binding.queueFamily;
	CheckVulkan(vkCreateCommandPool(native, &pool, nullptr, &m_resources->pool),
				"Create Vulkan submission command pool");
	VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocate.commandPool = m_resources->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	CheckVulkan(vkAllocateCommandBuffers(native, &allocate, &m_resources->command),
				"Allocate Vulkan submission commands");
	VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CheckVulkan(vkCreateFence(native, &fence, nullptr, &m_resources->fence), "Create Vulkan producer completion");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CheckVulkan(vkBeginCommandBuffer(m_resources->command, &begin), "Begin Vulkan submission commands");
}

VulkanSubmission::~VulkanSubmission()
{
	if (!NativeIdle())
	{
		std::fputs(
			"SimXR: pending Vulkan submission abandoned without confirmed completion; preserving native owners\n",
			stderr);
		m_resources->preservation.Preserve(std::move(m_resources));
	}
}

CompletionStatus VulkanSubmission::PollCompletion() const noexcept
{
	const auto cached = m_status.load();
	if (cached != CompletionStatus::Pending || !m_ready.load())
		return cached;
	if (m_resources->completion)
	{
		const auto completion = m_resources->completion->PollCompletion();
		if (completion != CompletionStatus::Pending)
		{
			auto expected = CompletionStatus::Pending;
			m_status.compare_exchange_strong(expected, completion);
		}
		return m_status.load();
	}
	const auto result = vkGetFenceStatus(m_resources->device->binding.device, m_resources->fence);
	if (result == VK_SUCCESS)
	{
		auto expected = CompletionStatus::Pending;
		m_status.compare_exchange_strong(expected, CompletionStatus::Complete);
	}
	else if (result != VK_NOT_READY)
	{
		if (result == VK_ERROR_DEVICE_LOST)
			m_resources->device->lost.store(true);
		auto expected = CompletionStatus::Pending;
		m_status.compare_exchange_strong(expected, CompletionStatus::Failed);
	}
	return m_status.load();
}

bool VulkanSubmission::NativeIdle() const noexcept
{
	if (!m_attempted.load())
		return true;
	if (!m_ready.load())
		return false;
	if (m_resources->device->lost.load())
		return true;
	const auto result = m_submitResult.load();
	if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
		return true;
	if (m_resources->completion)
		return m_resources->completion->NativeIdle();
	return vkGetFenceStatus(m_resources->device->binding.device, m_resources->fence) == VK_SUCCESS;
}

VkResult VulkanSubmission::Wait(std::uint64_t timeout) const noexcept
{
	if (!m_ready.load())
		return VK_NOT_READY;
	const auto submitted = m_submitResult.load();
	if (submitted == VK_ERROR_OUT_OF_HOST_MEMORY || submitted == VK_ERROR_OUT_OF_DEVICE_MEMORY)
		return submitted;
	if (m_resources->completion)
		return m_resources->completion->Wait(timeout);
	const auto result = vkWaitForFences(m_resources->device->binding.device, 1, &m_resources->fence, VK_TRUE, timeout);
	if (result == VK_SUCCESS)
	{
		auto expected = CompletionStatus::Pending;
		m_status.compare_exchange_strong(expected, CompletionStatus::Complete);
	}
	else if (result == VK_ERROR_DEVICE_LOST)
	{
		m_resources->device->lost.store(true);
		auto expected = CompletionStatus::Pending;
		m_status.compare_exchange_strong(expected, CompletionStatus::Failed);
	}
	return result;
}

void VulkanSubmission::ReleaseSources()
{
	if (!NativeIdle())
		return;
	const auto native = m_resources->device->binding.device;
	if (m_resources->descriptors)
	{
		vkDestroyDescriptorPool(native, m_resources->descriptors, nullptr);
		m_resources->descriptors = VK_NULL_HANDLE;
	}
	for (auto view : m_resources->sourceViews)
		vkDestroyImageView(native, view, nullptr);
	m_resources->sourceViews.clear();
	m_resources->sources.clear();
	m_resources->sourceBudgets.clear();
}

std::shared_ptr<VulkanImage> VulkanFrame::RetainImage() const
{
	return {m_submission, m_submission->m_resources->output.get()};
}

VulkanFrame::VulkanFrame(std::shared_ptr<VulkanSubmission> submission, std::span<const FrameView> views)
	: FrameOutput(views), m_submission(std::move(submission)) {}

CompletionStatus VulkanFrame::PollCompletion() const noexcept
{
	return m_submission->PollCompletion();
}

const VulkanImage& VulkanFrame::Image() const
{
	return *m_submission->m_resources->output;
}

VkImageView VulkanFrame::View(std::uint32_t eye) const
{
	if (eye >= Image().layers)
		throw std::out_of_range("Vulkan frame slice");
	return m_submission->m_resources->outputViews.at(eye);
}

std::shared_ptr<const GpuCompletion> VulkanFrame::Completion() const
{
	return m_submission;
}

std::shared_ptr<VulkanExternalImage> VulkanFrame::ExternalImage() const
{
	return m_submission->m_resources->external;
}

UniqueFd VulkanFrame::ExportReady()
{
	std::lock_guard lock(m_inspectionMutex);
	const auto& ready = m_submission->m_resources->ready;
	if (!ready)
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Frame has no external GPU readiness dependency");
	if (!m_readyFd)
		m_readyFd = ready->ExportOnce();
	return m_readyFd.Duplicate();
}

bool VulkanFrame::ClaimInspection()
{
	std::lock_guard lock(m_inspectionMutex);
	if (m_inspection)
		return false;
	m_inspection = 1;
	return true;
}

void VulkanFrame::CancelInspection() noexcept
{
	std::lock_guard lock(m_inspectionMutex);
	if (m_inspection == 1)
		m_inspection = 0;
}

void VulkanFrame::CommitInspection() noexcept
{
	std::lock_guard lock(m_inspectionMutex);
	if (m_inspection == 1)
		m_inspection = 2;
}

std::shared_ptr<FrameReservation> VulkanFrame::Reservation() const
{
	return m_submission->m_resources->reservation;
}

bool VulkanFrame::Submitted() const noexcept
{
	return m_submission->m_attempted.load();
}
} // namespace simxr
