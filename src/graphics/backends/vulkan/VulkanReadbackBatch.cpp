#include "graphics/backends/vulkan/VulkanReadbackBatch.h"
#include "graphics/backends/vulkan/VulkanNativePreservation.h"

#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace simxr
{
namespace
{
bool ReadbackFormat(VkFormat format)
{
	return format == VK_FORMAT_R8G8B8A8_UNORM || format == VK_FORMAT_R8G8B8A8_SRGB ||
		   format == VK_FORMAT_B8G8R8A8_UNORM || format == VK_FORMAT_B8G8R8A8_SRGB;
}
std::size_t ValidateRegion(const VulkanReadbackRegion& region, const std::shared_ptr<VulkanDevice>& device)
{
	const auto& image = region.image;
	if (!image || image->device != device || !ReadbackFormat(image->format) ||
		!(image->usage & VK_IMAGE_USAGE_TRANSFER_SRC_BIT) || region.layer >= image->layers || region.offset.x < 0 ||
		region.offset.y < 0 || !region.extent.width || !region.extent.height ||
		region.extent.width > image->extent.width || region.extent.height > image->extent.height ||
		static_cast<std::uint32_t>(region.offset.x) > image->extent.width - region.extent.width ||
		static_cast<std::uint32_t>(region.offset.y) > image->extent.height - region.extent.height)
		throw std::invalid_argument("Invalid Vulkan readback source or region");
	const auto pixels = std::uint64_t{region.extent.width} * region.extent.height;
	if (pixels > std::numeric_limits<std::size_t>::max() / 4)
		throw std::length_error("Vulkan readback region exceeds addressable storage");
	return static_cast<std::size_t>(pixels * 4);
}
class ReadbackMapping
{
  public:
	ReadbackMapping(VkDevice device, VkDeviceMemory memory, VkDeviceSize bytes) : m_device(device), m_memory(memory)
	{
		CheckVulkan(vkMapMemory(device, memory, 0, bytes, 0, &m_data), "Map explicit Vulkan readback");
	}
	~ReadbackMapping()
	{
		vkUnmapMemory(m_device, m_memory);
	}
	const void* Data() const noexcept
	{
		return m_data;
	}
	ReadbackMapping(const ReadbackMapping&) = delete;
	ReadbackMapping& operator=(const ReadbackMapping&) = delete;

  private:
	VkDevice m_device;
	VkDeviceMemory m_memory;
	void* m_data = nullptr;
};
} // namespace

struct VulkanReadbackBatch::Native
{
	struct Source
	{
		std::shared_ptr<VulkanImage> image;
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		std::size_t bytes = 0;
	};
	VulkanNativePreservation<Native, 4>::Reservation preservation{
		"Four explicit readback batches are retained across all application devices"};
	std::shared_ptr<VulkanDevice> device;
	std::vector<Source> sources;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	explicit Native(std::shared_ptr<VulkanDevice> owner) : device(std::move(owner)) {}
	~Native()
	{
		const auto native = device->binding.device;
		if (pool)
			vkDestroyCommandPool(native, pool, nullptr);
		if (fence)
			vkDestroyFence(native, fence, nullptr);
		for (const auto& source : sources)
		{
			if (source.buffer)
				vkDestroyBuffer(native, source.buffer, nullptr);
			if (source.memory)
				vkFreeMemory(native, source.memory, nullptr);
		}
	}
};

VulkanReadbackBatch::VulkanReadbackBatch(std::shared_ptr<VulkanDevice> device)
	: m_native(std::make_unique<Native>(std::move(device)))
{
}
std::shared_ptr<VulkanReadbackBatch> VulkanReadbackBatch::Prepare(std::shared_ptr<VulkanDevice> device,
																  std::span<const VulkanReadbackRegion> regions)
{
	if (!device || regions.empty() || regions.size() > 64)
		throw std::invalid_argument("Vulkan readback requires a device and one through sixty-four regions");
	auto batch = std::shared_ptr<VulkanReadbackBatch>(new VulkanReadbackBatch(std::move(device)));
	batch->Record(regions);
	return batch;
}
VulkanReadbackBatch::~VulkanReadbackBatch()
{
	if (!NativeIdle())
	{
		std::fputs("SimXR: explicit readback completion unknown; preserving charged native owners\n", stderr);
		m_native->preservation.Preserve(std::move(m_native));
	}
}
void VulkanReadbackBatch::Record(std::span<const VulkanReadbackRegion> regions)
{
	const auto& device = m_native->device;
	const auto native = device->binding.device;
	m_native->sources.reserve(regions.size());
	for (const auto& region : regions)
	{
		const auto bytes = ValidateRegion(region, device);
		m_native->sources.push_back({region.image, {}, {}, bytes});
		auto& source = m_native->sources.back();
		VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		buffer.size = bytes;
		buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		CheckVulkan(vkCreateBuffer(native, &buffer, nullptr, &source.buffer), "Create explicit readback buffer");
		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(native, source.buffer, &requirements);
		VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocation.allocationSize = requirements.size;
		allocation.memoryTypeIndex = device->MemoryType(
			requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		CheckVulkan(vkAllocateMemory(native, &allocation, nullptr, &source.memory),
					"Allocate explicit readback memory");
		CheckVulkan(vkBindBufferMemory(native, source.buffer, source.memory, 0), "Bind explicit readback buffer");
	}
	VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool.queueFamilyIndex = device->binding.queueFamily;
	CheckVulkan(vkCreateCommandPool(native, &pool, nullptr, &m_native->pool), "Create explicit readback commands");
	VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocate.commandPool = m_native->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	CheckVulkan(vkAllocateCommandBuffers(native, &allocate, &m_native->command), "Allocate explicit readback commands");
	VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CheckVulkan(vkCreateFence(native, &fence, nullptr, &m_native->fence), "Create explicit readback completion");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CheckVulkan(vkBeginCommandBuffer(m_native->command, &begin), "Begin explicit readback commands");
	for (std::size_t index = 0; index < regions.size(); ++index)
	{
		const auto& region = regions[index];
		VulkanImageBarrier(m_native->command, *region.image, region.image->ApplicationLayout(),
						   VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_ACCESS_MEMORY_WRITE_BIT,
						   VK_ACCESS_TRANSFER_READ_BIT);
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, region.layer, 1};
		copy.imageOffset = {region.offset.x, region.offset.y, 0};
		copy.imageExtent = {region.extent.width, region.extent.height, 1};
		vkCmdCopyImageToBuffer(m_native->command, region.image->image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
							   m_native->sources[index].buffer, 1, &copy);
		VulkanImageBarrier(m_native->command, *region.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   region.image->ApplicationLayout(), VK_ACCESS_TRANSFER_READ_BIT,
						   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT);
	}
	CheckVulkan(vkEndCommandBuffer(m_native->command), "End explicit readback commands");
}
VkResult VulkanReadbackBatch::Submit() noexcept
{
	auto expected = Phase::Prepared;
	if (!m_phase.compare_exchange_strong(expected, Phase::Submitting))
		return VK_ERROR_INITIALIZATION_FAILED;
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_native->command;
	const auto result = vkQueueSubmit(m_native->device->queue, 1, &submit, m_native->fence);
	m_submitResult.store(result);
	if (result == VK_SUCCESS)
		m_phase.store(Phase::Submitted);
	else
	{
		if (result == VK_ERROR_DEVICE_LOST)
			m_native->device->lost.store(true);
		m_status.store(CompletionStatus::Failed);
		const bool rejected = result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY;
		m_phase.store(rejected ? Phase::Rejected : Phase::Failed);
	}
	return result;
}
CompletionStatus VulkanReadbackBatch::PollCompletion() const noexcept
{
	const auto phase = m_phase.load();
	if (m_status.load() != CompletionStatus::Pending || phase == Phase::Prepared || phase == Phase::Submitting)
		return m_status.load();
	const auto result = vkGetFenceStatus(m_native->device->binding.device, m_native->fence);
	if (result != VK_NOT_READY)
	{
		if (result == VK_ERROR_DEVICE_LOST)
			m_native->device->lost.store(true);
		auto pending = CompletionStatus::Pending;
		m_status.compare_exchange_strong(pending,
										 result == VK_SUCCESS ? CompletionStatus::Complete : CompletionStatus::Failed);
	}
	return m_status.load();
}
bool VulkanReadbackBatch::NativeIdle() const noexcept
{
	const auto phase = m_phase.load();
	if (phase == Phase::Prepared || phase == Phase::Rejected)
		return true;
	if (phase == Phase::Submitting)
		return false;
	return m_native->device->lost.load() ||
		   vkGetFenceStatus(m_native->device->binding.device, m_native->fence) == VK_SUCCESS;
}
VkResult VulkanReadbackBatch::Wait(std::uint64_t timeout) const noexcept
{
	const auto phase = m_phase.load();
	if (phase == Phase::Prepared || phase == Phase::Submitting)
		return VK_NOT_READY;
	if (phase == Phase::Rejected || phase == Phase::Failed)
		return m_submitResult.load();
	const auto result = vkWaitForFences(m_native->device->binding.device, 1, &m_native->fence, VK_TRUE, timeout);
	if (result == VK_SUCCESS)
	{
		auto pending = CompletionStatus::Pending;
		m_status.compare_exchange_strong(pending, CompletionStatus::Complete);
	}
	else if (result == VK_ERROR_DEVICE_LOST)
	{
		m_native->device->lost.store(true);
		auto pending = CompletionStatus::Pending;
		m_status.compare_exchange_strong(pending, CompletionStatus::Failed);
	}
	return result;
}
std::vector<std::vector<std::uint8_t>> VulkanReadbackBatch::ReadPixels() const
{
	std::lock_guard lock(m_readMutex);
	if (PollCompletion() != CompletionStatus::Complete)
		throw std::logic_error("Explicit readback pixels require successful native completion");
	std::vector<std::vector<std::uint8_t>> result;
	result.reserve(m_native->sources.size());
	for (const auto& source : m_native->sources)
	{
		std::vector<std::uint8_t> pixels(source.bytes);
		ReadbackMapping mapped(m_native->device->binding.device, source.memory, source.bytes);
		std::memcpy(pixels.data(), mapped.Data(), source.bytes);
		result.push_back(std::move(pixels));
	}
	return result;
}
} // namespace simxr
