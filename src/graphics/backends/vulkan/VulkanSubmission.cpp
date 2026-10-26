#include "graphics/backends/vulkan/VulkanSubmission.h"
#include "Pch.h"

#include <cmath>
#include <cstdio>
#include <limits>

namespace simxr
{
VulkanSubmission::Resources::Resources(std::shared_ptr<VulkanDevice> owner) : device(std::move(owner)) {}

VulkanSubmission::Resources::~Resources()
{
	const auto native = device->binding.device;
	for (auto framebuffer : framebuffers)
		if (framebuffer)
			vkDestroyFramebuffer(native, framebuffer, nullptr);
	for (auto view : sourceViews)
		vkDestroyImageView(native, view, nullptr);
	for (auto view : outputViews)
		if (view)
			vkDestroyImageView(native, view, nullptr);
	device->RecycleCommands(std::move(commands));
}

VulkanSubmission::VulkanSubmission(std::shared_ptr<VulkanDevice> device)
	: m_resources(std::make_unique<Resources>(std::move(device)))
{
	m_resources->commands = m_resources->device->AcquireCommands();
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CheckVulkan(vkBeginCommandBuffer(m_resources->commands->command, &begin), "Begin Vulkan submission commands");
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

void VulkanSubmission::BeginTiming()
{
	const auto& device = *m_resources->device;
	if (!device.timestampBits || !std::isfinite(device.timestampPeriod) || device.timestampPeriod <= 0)
		return;
	VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO};
	query.queryType = VK_QUERY_TYPE_TIMESTAMP;
	query.queryCount = 2;
	const auto result = m_resources->commands->timestamps ? VK_SUCCESS
														  : vkCreateQueryPool(device.binding.device, &query, nullptr,
																			  &m_resources->commands->timestamps);
	if (result != VK_SUCCESS)
		return;
	m_resources->timing = std::make_shared<FrameGpuTiming>();
	vkCmdResetQueryPool(m_resources->commands->command, m_resources->commands->timestamps, 0, 2);
	vkCmdWriteTimestamp(m_resources->commands->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT,
						m_resources->commands->timestamps, 0);
}
void VulkanSubmission::EndTiming()
{
	if (m_resources->commands->timestamps)
		vkCmdWriteTimestamp(m_resources->commands->command, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
							m_resources->commands->timestamps, 1);
}
void VulkanSubmission::ObserveTiming() const noexcept
{
	const auto& resources = *m_resources;
	if (!resources.timing || resources.timing->compositorNanoseconds.load() >= 0)
		return;
	std::array<std::uint64_t, 2> values{};
	const auto result = vkGetQueryPoolResults(resources.device->binding.device, resources.commands->timestamps, 0, 2,
											  sizeof(values), values.data(), sizeof(values[0]), VK_QUERY_RESULT_64_BIT);
	if (result != VK_SUCCESS)
		return;
	const auto bits = resources.device->timestampBits;
	const auto mask = bits >= 64 ? ~std::uint64_t{0} : (std::uint64_t{1} << bits) - 1;
	const long double ns = static_cast<long double>((values[1] - values[0]) & mask) * resources.device->timestampPeriod;
	if (ns >= 0 && ns < static_cast<long double>(std::numeric_limits<std::int64_t>::max()))
		resources.timing->compositorNanoseconds.store(static_cast<std::int64_t>(ns));
}

CompletionStatus VulkanSubmission::PollCompletion() const noexcept
{
	const auto cached = m_status.load();
	if (cached != CompletionStatus::Pending || !m_ready.load())
	{
		if (cached == CompletionStatus::Complete)
			ObserveTiming();
		return cached;
	}
	if (m_resources->completion)
	{
		const auto completion = m_resources->completion->PollCompletion();
		if (completion != CompletionStatus::Pending)
		{
			auto expected = CompletionStatus::Pending;
			m_status.compare_exchange_strong(expected, completion);
		}
		if (m_status.load() == CompletionStatus::Complete)
			ObserveTiming();
		return m_status.load();
	}
	const auto result = vkGetFenceStatus(m_resources->device->binding.device, m_resources->commands->fence);
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
	if (m_status.load() == CompletionStatus::Complete)
		ObserveTiming();
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
	return vkGetFenceStatus(m_resources->device->binding.device, m_resources->commands->fence) == VK_SUCCESS;
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
	const auto result =
		vkWaitForFences(m_resources->device->binding.device, 1, &m_resources->commands->fence, VK_TRUE, timeout);
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
	for (auto view : m_resources->sourceViews)
		vkDestroyImageView(native, view, nullptr);
	m_resources->sourceViews.clear();
	m_resources->sources.clear();
	m_resources->sourceBudgets.clear();
#ifdef _WIN32
	m_resources->nativeSource.reset();
	m_resources->nativeReady = VK_NULL_HANDLE;
#endif
}

std::shared_ptr<VulkanImage> VulkanFrame::RetainImage() const
{
	return {m_submission, m_submission->m_resources->output.get()};
}

VulkanFrame::VulkanFrame(std::shared_ptr<VulkanSubmission> submission, std::span<const FrameView> views)
	: FrameOutput(views), m_submission(std::move(submission))
{
}

CompletionStatus VulkanFrame::PollCompletion() const noexcept
{
	return m_submission->PollCompletion();
}

std::shared_ptr<const FrameGpuTiming> VulkanFrame::Timing() const noexcept
{
	return m_submission->m_resources->timing;
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

VkResult VulkanFrame::Wait(std::uint64_t timeout) const noexcept
{
	return m_submission->Wait(timeout);
}

std::shared_ptr<VulkanExternalImage> VulkanFrame::ExternalImage() const
{
	return m_submission->m_resources->external;
}

VulkanExternalHandle VulkanFrame::ExportReady()
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
	if (m_inspection != InspectionClaim::Available)
		return false;
	m_inspection = InspectionClaim::Prepared;
	return true;
}

void VulkanFrame::CancelInspection() noexcept
{
	std::lock_guard lock(m_inspectionMutex);
	if (m_inspection == InspectionClaim::Prepared)
		m_inspection = InspectionClaim::Available;
}

void VulkanFrame::CommitInspection() noexcept
{
	std::lock_guard lock(m_inspectionMutex);
	if (m_inspection == InspectionClaim::Prepared)
		m_inspection = InspectionClaim::Committed;
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
