#include "control/backends/vulkan/VulkanImageConsumer.h"
#include "Pch.h"
#include "control/backends/vulkan/VulkanControlHandle.h"
#include "graphics/FramePool.h"
#include "graphics/SnapshotBudget.h"
#include "graphics/backends/vulkan/VulkanNativePreservation.h"

#include <cstdio>
#include <stdexcept>
#include <thread>

namespace simxr::control
{
static FramePool& ConsumerBudget()
{
	static FramePool s_budget{SnapshotBudget::Capacity};
	return s_budget;
}
static bool Rejected(VkResult result)
{
	return result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY;
}
static VulkanExternalImageDescriptor NativeDescription(const ImageDescription& value)
{
	constexpr auto allowed = VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT | VK_IMAGE_USAGE_SAMPLED_BIT |
							 VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT;
	if (value.handleKind != static_cast<std::uint32_t>(VulkanTransferKind) ||
		(value.format != VK_FORMAT_R8G8B8A8_UNORM && value.format != VK_FORMAT_R8G8B8A8_SRGB) || !value.width ||
		!value.height || value.depth != 1 || !value.arrayLayers || value.arrayLayers > 2 || value.mipLevels != 1 ||
		value.samples != 1 || value.tiling != VK_IMAGE_TILING_OPTIMAL || value.imageType != VK_IMAGE_TYPE_2D ||
		value.flags || value.layout != VK_IMAGE_LAYOUT_GENERAL || !value.allocationSize ||
		!(value.usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || (value.usage & ~allowed))
		throw std::invalid_argument("Unsupported native image format, shape, usage or handoff metadata");
	VulkanExternalImageDescriptor result;
	std::transform(value.identity.device.begin(), value.identity.device.end(), result.identity.deviceUuid.begin(),
				   [](auto byte)
				   {
					   return std::to_integer<std::uint8_t>(byte);
				   });
	std::transform(value.identity.driver.begin(), value.identity.driver.end(), result.identity.driverUuid.begin(),
				   [](auto byte)
				   {
					   return std::to_integer<std::uint8_t>(byte);
				   });
	result.image.format = static_cast<VkFormat>(value.format);
	result.image.extent = {value.width, value.height, 1};
	result.image.arrayLayers = value.arrayLayers;
	result.image.usage = value.usage;
	result.allocationSize = value.allocationSize;
	result.memoryTypeIndex = value.memoryTypeIndex;
	result.dedicated = value.dedicated;
	result.producerQueueFamily = value.producerQueueFamily;
	result.producerLayout = VK_IMAGE_LAYOUT_GENERAL;
	return result;
}
// Pending native submissions preserve this entire dependent resource owner.
struct VulkanImageConsumer::State
{
	VulkanNativePreservation<State, SnapshotBudget::MaxCapacity>::Reservation preservation{
		"Native consumer preservation budget exhausted"};
	std::shared_ptr<FrameReservation> slot;
	std::shared_ptr<VulkanDevice> device;
	VulkanExternalDevice external;
	std::shared_ptr<VulkanExternalFence> completion;
	std::shared_ptr<VulkanExternalTimeline> timelineCompletion;
	std::shared_ptr<TransferHandle> offered;
	std::shared_ptr<VulkanExternalImage> imported;
	std::shared_ptr<VulkanExternalSemaphore> ready;
	std::shared_ptr<VulkanImage> image;
	ImageDescription description;
	std::vector<VkImageView> views;
	VkSampler sampler = VK_NULL_HANDLE;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkSemaphore sampleReady = VK_NULL_HANDLE;
	VkFence retirement = VK_NULL_HANDLE;
	bool prepared = false, submitted = false, markerSubmitted = false, markerUnknown = false, idle = false,
		 cancelled = false, failed = false;
	State(std::shared_ptr<VulkanDevice> owner, std::span<const char* const> extensions, std::uint32_t api,
		  ImageCompletionKind kind, bool timelineEnabled)
		: slot(ConsumerBudget().Reserve()), device(std::move(owner)), external(device, extensions, api, timelineEnabled)
	{
		if (!slot || !ConsumerBudget().Consume(slot))
			throw VulkanError(VK_ERROR_OUT_OF_POOL_MEMORY, "Native consumer snapshot budget exhausted");
		if (kind == ImageCompletionKind::Timeline)
			timelineCompletion = external.CreateExportableTimeline();
		else if (kind == ImageCompletionKind::Fence)
			completion = external.CreateExportableFence();
		else
			throw std::invalid_argument("Unknown consumer completion kind");
		VkFenceCreateInfo create{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		CheckVulkan(vkCreateFence(device->binding.device, &create, nullptr, &retirement), "create consumer retirement");
	}
	~State()
	{
		for (auto view : views)
			vkDestroyImageView(device->binding.device, view, nullptr);
		if (sampler)
			vkDestroySampler(device->binding.device, sampler, nullptr);
		if (sampleReady)
			vkDestroySemaphore(device->binding.device, sampleReady, nullptr);
		if (pool)
			vkDestroyCommandPool(device->binding.device, pool, nullptr);
		if (retirement)
			vkDestroyFence(device->binding.device, retirement, nullptr);
	}
	VkResult Submit(VkSubmitInfo submit)
	{
		if (completion)
			return vkQueueSubmit(device->queue, 1, &submit, completion->Get());
		const std::array signals{sampleReady, timelineCompletion->Get()};
		const std::array<std::uint64_t, 2> values{0, 1};
		const bool hasSample = submit.signalSemaphoreCount != 0;
		VkTimelineSemaphoreSubmitInfo timeline{VK_STRUCTURE_TYPE_TIMELINE_SEMAPHORE_SUBMIT_INFO};
		timeline.waitSemaphoreValueCount = submit.waitSemaphoreCount;
		timeline.pWaitSemaphoreValues = values.data();
		timeline.signalSemaphoreValueCount = hasSample ? 2 : 1;
		timeline.pSignalSemaphoreValues = values.data() + (hasSample ? 0 : 1);
		submit.signalSemaphoreCount = timeline.signalSemaphoreValueCount;
		submit.pSignalSemaphores = signals.data() + (hasSample ? 0 : 1);
		submit.pNext = &timeline;
		return vkQueueSubmit(device->queue, 1, &submit, VK_NULL_HANDLE);
	}

	void Marker()
	{
		if (markerSubmitted || markerUnknown || !submitted || idle)
			return;
		VkSubmitInfo marker{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		const auto result = vkQueueSubmit(device->queue, 1, &marker, retirement);
		if (result == VK_SUCCESS)
			markerSubmitted = true;
		else if (!Rejected(result))
		{
			markerUnknown = true;
			failed = true;
			if (result == VK_ERROR_DEVICE_LOST)
				device->lost = true;
		}
	}
	CompletionStatus Poll()
	{
		if (device->lost)
		{
			idle = failed = true;
			return CompletionStatus::Failed;
		}
		if (!submitted)
			return failed ? CompletionStatus::Failed : CompletionStatus::Pending;
		Marker();
		if (!markerSubmitted)
			return failed ? CompletionStatus::Failed : CompletionStatus::Pending;
		const auto result = vkGetFenceStatus(device->binding.device, retirement);
		if (result == VK_SUCCESS)
		{
			idle = true;
			imported.reset();
			ready.reset();
			return failed || cancelled ? CompletionStatus::Failed : CompletionStatus::Complete;
		}
		if (result != VK_NOT_READY)
		{
			failed = true;
			if (result == VK_ERROR_DEVICE_LOST)
				device->lost = idle = true;
			return CompletionStatus::Failed;
		}
		return CompletionStatus::Pending;
	}
};

VulkanImageConsumer::VulkanImageConsumer(std::shared_ptr<VulkanDevice> device, std::span<const char* const> extensions,
										 std::uint32_t api, ImageCompletionKind completion, bool timelineEnabled)
	: m_state(std::make_unique<State>(std::move(device), extensions, api, completion, timelineEnabled))
{
}
VulkanImageConsumer::~VulkanImageConsumer()
{
	try
	{
		if (!m_state->submitted && m_state->offered)
			CancelUnused();
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(5);
		while (m_state->submitted && !m_state->idle && std::chrono::steady_clock::now() < deadline)
		{
			m_state->Poll();
			if (!m_state->idle)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
		}
	}
	catch (...)
	{
	}
	if (m_state->submitted && !m_state->idle)
	{
		std::fputs("SimXR: native consumer completion unknown; preserving bounded owners and device\n", stderr);
		m_state->preservation.Preserve(std::move(m_state));
	}
}
ImageDeviceIdentity VulkanImageConsumer::Identity() const
{
	ImageDeviceIdentity result;
	const auto& native = m_state->external.Identity();
	std::transform(native.deviceUuid.begin(), native.deviceUuid.end(), result.device.begin(),
				   [](auto byte)
				   {
					   return static_cast<std::byte>(byte);
				   });
	std::transform(native.driverUuid.begin(), native.driverUuid.end(), result.driver.begin(),
				   [](auto byte)
				   {
					   return static_cast<std::byte>(byte);
				   });
	return result;
}
std::shared_ptr<TransferHandle> VulkanImageConsumer::ConsumerFence()
{
	if (!m_state->offered)
	{
		auto fd = m_state->completion ? m_state->completion->ExportOnce() : m_state->timelineCompletion->ExportOnce();
		m_state->offered = OwnVulkanControlHandle(std::move(fd));
	}
	return m_state->offered;
}
void VulkanImageConsumer::Import(const ImageDescription& description,
								 std::span<const std::shared_ptr<TransferHandle>> handles)
{
	if (m_state->image || m_state->submitted || handles.size() != 2 || description.identity != Identity())
		throw std::invalid_argument("Image import requires one compatible delivery and two native handles");
	if ((description.completionKind == ImageCompletionKind::Timeline) != bool(m_state->timelineCompletion))
		throw std::invalid_argument("Delivered image changed its consumer completion kind");
	const auto native = NativeDescription(description);
	SnapshotBudget::ObserveImageBytes(description.allocationSize);
	if (ConsumerBudget().InUse() > ConsumerBudget().Capacity())
		throw VulkanError(VK_ERROR_OUT_OF_POOL_MEMORY, "Snapshot image exceeds current retention capacity");
	// TransferHandle has shared ownership. Duplicate into independent native RAII
	// before a Vulkan helper consumes its own successful import ownership.
	auto memory = DuplicateVulkanControlHandle(handles[0]);
	auto ready = DuplicateVulkanControlHandle(handles[1]);
	m_state->imported = m_state->external.ImportImage(native, memory);
	m_state->ready = m_state->external.ImportSemaphore(native.identity, ready);
	m_state->description = description;
	m_state->image = VulkanImage::Create(m_state->device, static_cast<VkFormat>(description.format),
										 {description.width, description.height}, description.arrayLayers,
										 VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT |
											 VK_IMAGE_USAGE_SAMPLED_BIT);
	m_state->views.reserve(description.arrayLayers);
	for (std::uint32_t layer = 0; layer < description.arrayLayers; ++layer)
	{
		VkImageViewCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		create.image = m_state->image->image;
		create.viewType = VK_IMAGE_VIEW_TYPE_2D;
		create.format = m_state->image->format;
		create.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, layer, 1};
		VkImageView view;
		CheckVulkan(vkCreateImageView(m_state->device->binding.device, &create, nullptr, &view),
					"create consumer slice view");
		m_state->views.push_back(view);
	}
	VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampler.magFilter = sampler.minFilter = VK_FILTER_LINEAR;
	sampler.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	CheckVulkan(vkCreateSampler(m_state->device->binding.device, &sampler, nullptr, &m_state->sampler),
				"create consumer sampler");
	VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	CheckVulkan(vkCreateSemaphore(m_state->device->binding.device, &semaphore, nullptr, &m_state->sampleReady),
				"create consumer sampling dependency");
	VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool.queueFamilyIndex = m_state->device->binding.queueFamily;
	CheckVulkan(vkCreateCommandPool(m_state->device->binding.device, &pool, nullptr, &m_state->pool),
				"create consumer copy pool");
	VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocate.commandPool = m_state->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	CheckVulkan(vkAllocateCommandBuffers(m_state->device->binding.device, &allocate, &m_state->command),
				"allocate consumer copy");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CheckVulkan(vkBeginCommandBuffer(m_state->command, &begin), "record consumer copy");
	VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	acquire.oldLayout = acquire.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	acquire.dstQueueFamilyIndex = m_state->device->binding.queueFamily;
	acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	acquire.image = m_state->imported->Image()->image;
	acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, description.arrayLayers};
	vkCmdPipelineBarrier(m_state->command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
						 nullptr, 0, nullptr, 1, &acquire);
	VulkanImageBarrier(m_state->command, *m_state->image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
					   VK_ACCESS_TRANSFER_WRITE_BIT);
	VkImageCopy copy{};
	copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, description.arrayLayers};
	copy.extent = {description.width, description.height, 1};
	vkCmdCopyImage(m_state->command, acquire.image, VK_IMAGE_LAYOUT_GENERAL, m_state->image->image,
				   VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	auto release = acquire;
	release.srcQueueFamilyIndex = acquire.dstQueueFamilyIndex;
	release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	release.dstAccessMask = 0;
	vkCmdPipelineBarrier(m_state->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
						 nullptr, 0, nullptr, 1, &release);
	VulkanImageBarrier(m_state->command, *m_state->image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
					   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_SHADER_READ_BIT);
	CheckVulkan(vkEndCommandBuffer(m_state->command), "finish consumer copy");
	m_state->prepared = true;
}
void VulkanImageConsumer::Submit()
{
	if (m_state->submitted || !m_state->prepared)
		throw std::logic_error("Native image consumer is not prepared for one submission");
	const auto ready = m_state->ready->Get();
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &ready;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_state->command;
	submit.signalSemaphoreCount = 1;
	submit.pSignalSemaphores = &m_state->sampleReady;
	const auto result = m_state->Submit(submit);
	m_state->submitted = result == VK_SUCCESS || !Rejected(result);
	m_state->failed = result != VK_SUCCESS;
	if (result == VK_ERROR_DEVICE_LOST)
		m_state->device->lost = true;
	CheckVulkan(result, "submit native consumer copy");
	m_state->Marker();
}
void VulkanImageConsumer::AbandonUnsent()
{
	if (m_state->submitted)
		throw std::logic_error("Submitted consumer work cannot be abandoned as unsent");
	m_state->offered.reset();
}
CompletionStatus VulkanImageConsumer::Poll()
{
	return m_state->Poll();
}
bool VulkanImageConsumer::NativeIdle() const
{
	return m_state->idle || (!m_state->submitted && !m_state->offered);
}
void VulkanImageConsumer::CancelUnused()
{
	if (m_state->submitted)
		return;
	VkSubmitInfo empty{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	const auto result = m_state->Submit(empty);
	m_state->submitted = result == VK_SUCCESS || !Rejected(result);
	m_state->cancelled = true;
	if (result == VK_ERROR_DEVICE_LOST)
		m_state->device->lost = true;
	CheckVulkan(result, "acknowledge unused consumer delivery");
	m_state->Marker();
}
VkSemaphore VulkanImageConsumer::SampleReady() const
{
	return m_state->sampleReady;
}
std::span<const VkImageView> VulkanImageConsumer::Views() const
{
	return m_state->views;
}
VkSampler VulkanImageConsumer::Sampler() const
{
	return m_state->sampler;
}
const ImageDescription& VulkanImageConsumer::Description() const
{
	return m_state->description;
}
std::uint64_t VulkanImageConsumer::AllocatedBytes() const noexcept
{
	return (m_state->image ? m_state->image->allocationSize : 0) +
		   (m_state->imported ? m_state->imported->Image()->allocationSize : 0);
}
const VulkanImage& VulkanImageConsumer::Image() const
{
	if (!m_state->image)
		throw std::logic_error("Native consumer has no imported private image");
	return *m_state->image;
}
} // namespace simxr::control
