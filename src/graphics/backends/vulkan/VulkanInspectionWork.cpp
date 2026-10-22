#include "graphics/backends/vulkan/VulkanInspectionWork.h"
#include "graphics/backends/vulkan/VulkanNativePreservation.h"
#include "graphics/backends/vulkan/VulkanSubmission.h"

#include <algorithm>
#include <cstdio>
#include <cstring>
#include <limits>
#include <utility>

namespace simxr
{
struct VulkanInspectionWork::Native
{
	VulkanNativePreservation<Native, 64>::Reservation preservation{
		"Sixty-four native inspection work owners are retained across all queue generations"};
	std::shared_ptr<VulkanDevice> device;
	std::shared_ptr<FrameReservation> slot;
	std::shared_ptr<VulkanFrame> producer;
	std::shared_ptr<VulkanExternalImage> imported;
	std::shared_ptr<VulkanExternalImage> externalOutput;
	std::shared_ptr<VulkanExternalSemaphore> ready;
	std::shared_ptr<VulkanExternalSemaphore> signal;
	std::shared_ptr<VulkanImage> sourceImage;
	std::shared_ptr<FrameReservation> sourceSlot;
	std::shared_ptr<const void> retirementOwner;
	std::shared_ptr<VulkanImage> image;
	VkCommandPool pool = VK_NULL_HANDLE;
	VkCommandBuffer command = VK_NULL_HANDLE;
	VkFence fence = VK_NULL_HANDLE;
	VkBuffer buffer = VK_NULL_HANDLE;
	VkDeviceMemory memory = VK_NULL_HANDLE;
	VkDeviceSize bytes = 0;
	std::uint32_t layers = 0;
	VkFormat pixelFormat = VK_FORMAT_UNDEFINED;
	VkExtent2D pixelExtent{};
	bool coherent = false;
	~Native()
	{
		const auto native = device->binding.device;
		if (pool)
			vkDestroyCommandPool(native, pool, nullptr);
		if (fence)
			vkDestroyFence(native, fence, nullptr);
		if (buffer)
			vkDestroyBuffer(native, buffer, nullptr);
		if (memory)
			vkFreeMemory(native, memory, nullptr);
	}
};

VulkanInspectionWork::VulkanInspectionWork(std::shared_ptr<VulkanDevice> device,
										   std::shared_ptr<FrameReservation> admission)
	: m_native(std::make_unique<Native>())
{
	m_native->device = std::move(device);
	m_native->slot = std::move(admission);
	const auto nativeDevice = m_native->device->binding.device;
	VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool.queueFamilyIndex = m_native->device->binding.queueFamily;
	CheckVulkan(vkCreateCommandPool(nativeDevice, &pool, nullptr, &m_native->pool), "Create inspection commands");
	VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocate.commandPool = m_native->pool;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	CheckVulkan(vkAllocateCommandBuffers(nativeDevice, &allocate, &m_native->command), "Allocate inspection commands");
	VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CheckVulkan(vkCreateFence(nativeDevice, &fence, nullptr, &m_native->fence), "Create inspection completion");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CheckVulkan(vkBeginCommandBuffer(m_native->command, &begin), "Begin inspection commands");
}
VulkanInspectionWork::~VulkanInspectionWork()
{
	if (!NativeIdle())
	{
		std::fputs("SimXR: inspection completion unknown; preserving native owners\n", stderr);
		m_native->preservation.Preserve(std::move(m_native));
	}
}
bool VulkanInspectionWork::Submitted() const noexcept
{
	return m_submitted.load();
}
bool VulkanInspectionWork::NativeIdle() const noexcept
{
	if (!m_submitted.load())
		return true;
	if (!m_ready.load())
		return false;
	return m_native->device->lost.load() ||
		   vkGetFenceStatus(m_native->device->binding.device, m_native->fence) == VK_SUCCESS;
}
CompletionStatus VulkanInspectionWork::PollCompletion() const noexcept
{
	if (m_status.load() != CompletionStatus::Pending || !m_ready.load())
		return m_status.load();
	const auto result = vkGetFenceStatus(m_native->device->binding.device, m_native->fence);
	if (result != VK_NOT_READY)
	{
		if (result == VK_ERROR_DEVICE_LOST)
			m_native->device->lost.store(true);
		auto expected = CompletionStatus::Pending;
		m_status.compare_exchange_strong(expected,
										 result == VK_SUCCESS ? CompletionStatus::Complete : CompletionStatus::Failed);
	}
	return m_status.load();
}
VkResult VulkanInspectionWork::Wait(std::uint64_t timeout) const noexcept
{
	if (!Submitted() || !m_ready.load())
		return VK_NOT_READY;
	const auto result = vkWaitForFences(m_native->device->binding.device, 1, &m_native->fence, VK_TRUE, timeout);
	if (result == VK_ERROR_DEVICE_LOST)
		m_native->device->lost.store(true);
	return result;
}
void VulkanInspectionWork::ReleaseSources()
{
	// Detach first; native-bearing last readers end outside the source mutex.
	std::shared_ptr<VulkanFrame> producer;
	std::shared_ptr<VulkanExternalImage> imported;
	std::shared_ptr<VulkanExternalSemaphore> ready;
	std::shared_ptr<VulkanImage> source;
	std::shared_ptr<VulkanImage> readbackImage;
	std::shared_ptr<FrameReservation> sourceBudget;
	{
		std::lock_guard lock(m_sourceMutex);
		if (!NativeIdle())
			return;
		producer = std::move(m_native->producer);
		imported = std::move(m_native->imported);
		ready = std::move(m_native->ready);
		sourceBudget = std::move(m_native->sourceSlot);
		source = std::move(m_native->sourceImage);
		if (m_native->buffer)
			readbackImage = std::move(m_native->image);
	}
}
CompletionStatus VulkanInspectionWork::ProducerCompletion() const noexcept
{
	return m_native->producer ? m_native->producer->PollCompletion() : CompletionStatus::Failed;
}
void VulkanInspectionWork::ImportProducerReady(const VulkanExternalDevice& external)
{
	auto ready = m_native->producer->ExportReady();
	m_native->ready = external.ImportSemaphore(m_native->producer->ExternalImage()->Descriptor().identity, ready);
}
void VulkanInspectionWork::CommitProducer() noexcept
{
	m_native->producer->CommitInspection();
}
void VulkanInspectionWork::CancelProducer() noexcept
{
	m_native->producer->CancelInspection();
}
const std::shared_ptr<VulkanImage>& VulkanInspectionWork::Image() const noexcept
{
	return m_native->image;
}
const std::shared_ptr<VulkanExternalImage>& VulkanInspectionWork::ExternalOutput() const noexcept
{
	return m_native->externalOutput;
}
const std::shared_ptr<VulkanExternalSemaphore>& VulkanInspectionWork::Signal() const noexcept
{
	return m_native->signal;
}

VkResult VulkanInspectionWork::Submit() noexcept
{
	if (m_native->device->lost.load())
	{
		m_status.store(CompletionStatus::Failed);
		return VK_ERROR_DEVICE_LOST;
	}
	m_submitted.store(true);
	const auto ready = m_native->ready ? m_native->ready->Get() : VK_NULL_HANDLE;
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.waitSemaphoreCount = ready ? 1 : 0;
	submit.pWaitSemaphores = &ready;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_native->command;
	const auto signal = m_native->signal ? m_native->signal->Get() : VK_NULL_HANDLE;
	submit.signalSemaphoreCount = signal ? 1 : 0;
	submit.pSignalSemaphores = &signal;
	const auto result = vkQueueSubmit(m_native->device->queue, 1, &submit, m_native->fence);
	if (result != VK_SUCCESS)
	{
		if (result == VK_ERROR_OUT_OF_HOST_MEMORY || result == VK_ERROR_OUT_OF_DEVICE_MEMORY)
			m_submitted.store(false);
		if (result == VK_ERROR_DEVICE_LOST)
			m_native->device->lost.store(true);
		m_status.store(CompletionStatus::Failed);
	}
	m_ready.store(true);
	return result;
}
std::shared_ptr<const VulkanImagePixels> VulkanInspectionWork::ReadPixels() const
{
	if (!m_native->buffer || PollCompletion() != CompletionStatus::Complete)
		return {};
	auto pixels = std::make_shared<VulkanImagePixels>();
	pixels->format = m_native->pixelFormat;
	pixels->width = m_native->pixelExtent.width;
	pixels->height = m_native->pixelExtent.height;
	pixels->layers = m_native->layers;
	pixels->rgba.resize(static_cast<std::size_t>(m_native->bytes));
	void* mapped = nullptr;
	const auto device = m_native->device->binding.device;
	CheckVulkan(vkMapMemory(device, m_native->memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Map explicit inspection export");
	struct Mapping
	{
		VkDevice device;
		VkDeviceMemory memory;
		~Mapping()
		{
			vkUnmapMemory(device, memory);
		}
	} mapping{device, m_native->memory};
	if (!m_native->coherent)
	{
		VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
		range.memory = m_native->memory;
		range.size = VK_WHOLE_SIZE;
		CheckVulkan(vkInvalidateMappedMemoryRanges(device, 1, &range), "Invalidate explicit inspection export");
	}
	std::memcpy(pixels->rgba.data(), mapped, pixels->rgba.size());
	return pixels;
}
std::shared_ptr<VulkanInspectionWork> VulkanInspectionWork::Import(std::shared_ptr<VulkanDevice> device,
																   const VulkanExternalDevice& external,
																   std::shared_ptr<FrameReservation> admission,
																   std::shared_ptr<VulkanFrame> producer)
{
	auto work = std::shared_ptr<VulkanInspectionWork>(new VulkanInspectionWork(device, std::move(admission)));
	work->m_native->producer = producer;
	const auto image = producer->ExternalImage();
	auto memory = image->ExportMemory();
	work->m_native->imported = external.ImportImage(image->Descriptor(), memory);
	const auto& descriptor = image->Descriptor();
	const auto& spec = descriptor.image;
	auto destination = VulkanImage::Create(
		device, spec.format, {spec.extent.width, spec.extent.height}, spec.arrayLayers,
		VK_IMAGE_USAGE_TRANSFER_SRC_BIT | VK_IMAGE_USAGE_TRANSFER_DST_BIT | VK_IMAGE_USAGE_SAMPLED_BIT, spec.mipLevels);
	work->m_native->image = destination;
	const auto& imported = *work->m_native->imported->Image();
	VkImageMemoryBarrier acquire{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	acquire.oldLayout = acquire.newLayout = descriptor.producerLayout;
	acquire.srcQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	acquire.dstQueueFamilyIndex = device->binding.queueFamily;
	acquire.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	acquire.image = imported.image;
	acquire.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, spec.mipLevels, 0, spec.arrayLayers};
	vkCmdPipelineBarrier(work->m_native->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
						 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &acquire);
	VulkanImageBarrier(work->m_native->command, *destination, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
					   VK_ACCESS_TRANSFER_WRITE_BIT);
	VkImageCopy copy{};
	copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, spec.arrayLayers};
	copy.dstSubresource = copy.srcSubresource;
	for (std::uint32_t mip = 0; mip < spec.mipLevels; ++mip)
	{
		copy.srcSubresource.mipLevel = copy.dstSubresource.mipLevel = mip;
		copy.extent = {std::max(1U, spec.extent.width >> mip), std::max(1U, spec.extent.height >> mip), 1};
		vkCmdCopyImage(work->m_native->command, imported.image, acquire.newLayout, destination->image,
			VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	}
	VkImageMemoryBarrier release = acquire;
	release.srcQueueFamilyIndex = acquire.dstQueueFamilyIndex;
	release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	release.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	release.dstAccessMask = 0;
	vkCmdPipelineBarrier(work->m_native->command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT,
						 VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0, nullptr, 1, &release);
	VulkanImageBarrier(work->m_native->command, *destination, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL,
					   VK_ACCESS_TRANSFER_WRITE_BIT, VK_ACCESS_MEMORY_READ_BIT);
	CheckVulkan(vkEndCommandBuffer(work->m_native->command), "Record retained inspection copy");
	return work;
}
std::shared_ptr<VulkanInspectionWork>
VulkanInspectionWork::Readback(std::shared_ptr<VulkanDevice> deviceOwner, std::shared_ptr<FrameReservation> admission,
							   std::shared_ptr<VulkanImage> source, std::shared_ptr<FrameReservation> sourceBudget,
							   std::optional<std::uint32_t> layer, std::shared_ptr<const void> retirementOwner)
{
	const auto& image = *source;
	if (layer && *layer >= image.layers)
		throw std::invalid_argument("Invalid inspection export layer");
	const auto layers = layer ? 1U : image.layers;
	const std::uint64_t bytes = static_cast<std::uint64_t>(image.extent.width) * image.extent.height * layers * 4;
	if (!bytes || bytes > 512ULL * 1024 * 1024 || bytes > std::numeric_limits<std::size_t>::max())
		throw VulkanError(VK_ERROR_OUT_OF_HOST_MEMORY, "Inspection export exceeds bounded byte capacity");
	auto work =
		std::shared_ptr<VulkanInspectionWork>(new VulkanInspectionWork(std::move(deviceOwner), std::move(admission)));
	work->m_native->retirementOwner = std::move(retirementOwner);
	work->m_native->image = source;
	work->m_native->sourceSlot = std::move(sourceBudget);
	work->m_native->pixelFormat = image.format;
	work->m_native->pixelExtent = image.extent;
	work->m_native->layers = layers;
	work->m_native->bytes = bytes;
	const auto device = image.device->binding.device;
	VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buffer.size = bytes;
	buffer.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	CheckVulkan(vkCreateBuffer(device, &buffer, nullptr, &work->m_native->buffer), "Create explicit export buffer");
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, work->m_native->buffer, &requirements);
	VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	memory.allocationSize = requirements.size;
	try
	{
		memory.memoryTypeIndex = image.device->MemoryType(
			requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
		work->m_native->coherent = true;
	}
	catch (const VulkanError&)
	{
		memory.memoryTypeIndex =
			image.device->MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT);
	}
	CheckVulkan(vkAllocateMemory(device, &memory, nullptr, &work->m_native->memory), "Allocate explicit export buffer");
	CheckVulkan(vkBindBufferMemory(device, work->m_native->buffer, work->m_native->memory, 0),
				"Bind explicit export buffer");
	VulkanImageBarrier(work->m_native->command, image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, layer.value_or(0), layers};
	copy.imageExtent = {image.extent.width, image.extent.height, 1};
	vkCmdCopyImageToBuffer(work->m_native->command, image.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
						   work->m_native->buffer, 1, &copy);
	VulkanImageBarrier(work->m_native->command, image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
					   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
	VkBufferMemoryBarrier host{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
	host.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	host.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	host.srcQueueFamilyIndex = host.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	host.buffer = work->m_native->buffer;
	host.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(work->m_native->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0,
						 nullptr, 1, &host, 0, nullptr);
	CheckVulkan(vkEndCommandBuffer(work->m_native->command), "Record explicit inspection export");
	return work;
}
std::shared_ptr<VulkanInspectionWork>
VulkanInspectionWork::Delivery(std::shared_ptr<VulkanDevice> device, const VulkanExternalDevice& external,
							   std::shared_ptr<FrameReservation> admission, std::shared_ptr<VulkanImage> sourceImage,
							   std::shared_ptr<FrameReservation> sourceBudget, const VulkanExternalImageSpec& spec)
{
	auto work = std::shared_ptr<VulkanInspectionWork>(new VulkanInspectionWork(device, std::move(admission)));
	work->m_native->externalOutput = external.CreateExportable(spec);
	work->m_native->signal = external.CreateExportableSemaphore();
	auto destination = work->m_native->externalOutput->Image();
	work->m_native->image = destination;
	work->m_native->sourceImage = sourceImage;
	work->m_native->sourceSlot = std::move(sourceBudget);
	const auto& source = *sourceImage;
	const auto& destinationImage = *destination;
	VulkanImageBarrier(work->m_native->command, source, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL,
					   VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT, VK_ACCESS_TRANSFER_READ_BIT);
	VulkanImageBarrier(work->m_native->command, destinationImage, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
					   VK_ACCESS_TRANSFER_WRITE_BIT);
	VkImageCopy copy{};
	copy.srcSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, source.layers};
	copy.dstSubresource = copy.srcSubresource;
	copy.extent = {source.extent.width, source.extent.height, 1};
	vkCmdCopyImage(work->m_native->command, source.image, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, destinationImage.image,
				   VK_IMAGE_LAYOUT_GENERAL, 1, &copy);
	VulkanImageBarrier(work->m_native->command, source, VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL, VK_IMAGE_LAYOUT_GENERAL,
					   VK_ACCESS_TRANSFER_READ_BIT, VK_ACCESS_MEMORY_READ_BIT);
	VkImageMemoryBarrier release{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	release.oldLayout = release.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	release.srcQueueFamilyIndex = device->binding.queueFamily;
	release.dstQueueFamilyIndex = VK_QUEUE_FAMILY_EXTERNAL;
	release.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	release.image = destinationImage.image;
	release.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, source.layers};
	vkCmdPipelineBarrier(work->m_native->command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT,
						 0, 0, nullptr, 0, nullptr, 1, &release);
	CheckVulkan(vkEndCommandBuffer(work->m_native->command), "Record shared image delivery");
	return work;
}
} // namespace simxr
