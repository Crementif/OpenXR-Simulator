#include "CompanionFrameReceiver.h"
#include "app/UiPch.h"
#include "VulkanProcessPeer.h"

#include <fcntl.h>
#include <poll.h>
#include <unistd.h>

namespace simxr::app
{
using namespace simxr::probe;

template<typename Function>
static Function DeviceFunction(VkDevice device, const char* name)
{
	auto function = reinterpret_cast<Function>(vkGetDeviceProcAddr(device, name));
	if (!function)
		throw std::runtime_error(std::string("Proof device extension command missing: ") + name);
	return function;
}

static bool Readable(int channel)
{
	pollfd status{channel, POLLIN, 0};
	int result;
	do
	{
		result = poll(&status, 1, 0);
	}
	while (result < 0 && errno == EINTR);
	if (result < 0)
		throw std::runtime_error("Companion proof channel poll failed");
	if (result && !(status.revents & POLLIN))
		throw std::runtime_error("Companion proof producer disconnected before completion");
	return result > 0;
}

CompanionFrameReceiver::CompanionFrameReceiver(VulkanDeviceAccess value, int socket)
	: m_access(value), m_channel(socket), m_ownedChannel(socket)
{
	if (!m_access.device || !m_access.physical || !m_access.ioQueue || m_channel < 0)
		throw std::invalid_argument("Companion proof requires an enabled import device and independent IO queue");
	const int flags = fcntl(m_channel, F_GETFL, 0);
	if (flags < 0 || fcntl(m_channel, F_SETFL, flags | O_NONBLOCK))
		throw std::runtime_error("Cannot configure nonblocking companion proof channel");
}

CompanionFrameReceiver::~CompanionFrameReceiver()
{
	Drain();
	for (auto& frame : m_frames)
	{
		if (frame.completion)
			vkDestroyFence(m_access.device, frame.completion, nullptr);
		if (frame.sampleReady)
			vkDestroySemaphore(m_access.device, frame.sampleReady, nullptr);
		Destroy(frame.output);
	}
	if (m_commands)
		vkDestroyCommandPool(m_access.device, m_commands, nullptr);
	if (m_sampler)
		vkDestroySampler(m_access.device, m_sampler, nullptr);
	if (m_ready)
		vkDestroySemaphore(m_access.device, m_ready, nullptr);
	if (m_released)
		vkDestroySemaphore(m_access.device, m_released, nullptr);
	Destroy(m_source);
}

void CompanionFrameReceiver::Poll()
{
	if (m_finished)
		return;
	if (!m_initialized)
	{
		if (!Readable(m_channel))
			return;
		Initialize();
	}
	for (std::uint32_t index = 0; index < m_received; ++index)
	{
		auto& frame = m_frames[index];
		if (frame.copied)
			continue;
		const auto status = vkGetFenceStatus(m_access.device, frame.completion);
		if (status == VK_NOT_READY)
			continue;
		Check(status, "observe companion private frame completion");
		sharing::Send(m_channel, &index, sizeof(index));
		frame.copied = true;
		std::printf("Companion proof frame=%u retained=private-GPU sourceReusable=yes\n", index + 1);
	}
	if (m_received < m_frames.size() && Readable(m_channel))
	{
		std::uint32_t frame = UINT32_MAX;
		sharing::Descriptors descriptors;
		sharing::Receive(m_channel, &frame, sizeof(frame), descriptors);
		if (frame != m_received)
			throw std::runtime_error("Companion proof frame identity mismatch");
		SubmitCopy(m_received++);
	}
}

std::vector<CompanionProofFrame> CompanionFrameReceiver::ReadyFrames() const
{
	std::vector<CompanionProofFrame> result;
	for (std::uint32_t index = 0; index < m_frames.size(); ++index)
		if (m_frames[index].copied)
			result.push_back({index + 1, m_frames[index].output.view, m_sampler, m_frames[index].sampleReady});
	return result;
}

bool CompanionFrameReceiver::AllCopied() const
{
	return std::all_of(m_frames.begin(), m_frames.end(), [](const auto& frame) { return frame.copied; });
}

void CompanionFrameReceiver::MarkSampled(std::uint32_t id)
{
	if (!id || id > m_frames.size() || !m_frames[id - 1].copied)
		throw std::invalid_argument("Companion proof sampled an unavailable image");
	m_frames[id - 1].sampled = true;
}

void CompanionFrameReceiver::Finish(bool verifyPixels)
{
	if (m_finished)
		return;
	if (!std::all_of(m_frames.begin(), m_frames.end(), [](const auto& frame) { return frame.sampled; }))
		throw std::logic_error("Companion proof has not sampled every retained image");
	if (verifyPixels)
		Verify();
	const std::uint32_t count = 3;
	sharing::Send(m_channel, &count, sizeof(count));
	m_finished = true;
	std::puts("Companion proof: three private GPU images retained and sampled after source reuse");
}

void CompanionFrameReceiver::Drain() noexcept
{
	for (const auto& frame : m_frames)
		if (frame.submitted &&
			vkWaitForFences(m_access.device, 1, &frame.completion, VK_TRUE, 5000000000ULL) != VK_SUCCESS)
		{
			std::fputs("FAIL: companion proof IO completion unknown; pending native resources remain owned\n", stderr);
			std::_Exit(1);
		}
}

void CompanionFrameReceiver::Destroy(Image& image)
{
	if (image.view)
		vkDestroyImageView(m_access.device, image.view, nullptr);
	if (image.image)
		vkDestroyImage(m_access.device, image.image, nullptr);
	if (image.memory)
		vkFreeMemory(m_access.device, image.memory, nullptr);
}

void CompanionFrameReceiver::CreateImage(Image& image, VkImageUsageFlags usage, bool external)
{
	VkExternalMemoryImageCreateInfo sharing{VK_STRUCTURE_TYPE_EXTERNAL_MEMORY_IMAGE_CREATE_INFO};
	sharing.handleTypes = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	VkImageCreateInfo info{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO};
	info.pNext = external ? &sharing : nullptr;
	info.imageType = VK_IMAGE_TYPE_2D;
	info.format = ImageFormat;
	info.extent = ImageExtent;
	info.mipLevels = info.arrayLayers = 1;
	info.samples = VK_SAMPLE_COUNT_1_BIT;
	info.tiling = VK_IMAGE_TILING_OPTIMAL;
	info.usage = usage;
	Check(vkCreateImage(m_access.device, &info, nullptr, &image.image), "create companion proof image");
}

void CompanionFrameReceiver::Allocate(Image& image, VkDeviceSize size, std::uint32_t type, int descriptor)
{
	FileDescriptor retained(descriptor);
	VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
	dedicated.image = image.image;
	VkImportMemoryFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_MEMORY_FD_INFO_KHR, &dedicated};
	imported.handleType = VK_EXTERNAL_MEMORY_HANDLE_TYPE_OPAQUE_FD_BIT;
	imported.fd = descriptor;
	VkMemoryAllocateInfo info{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	info.pNext = descriptor >= 0 ? static_cast<const void*>(&imported) : &dedicated;
	info.allocationSize = size;
	info.memoryTypeIndex = type;
	Check(vkAllocateMemory(m_access.device, &info, nullptr, &image.memory),
		  "allocate companion private frame or import source backing");
	retained.Release();
	Check(vkBindImageMemory(m_access.device, image.image, image.memory, 0), "bind companion proof memory");
}

VkSemaphore CompanionFrameReceiver::Semaphore(int descriptor)
{
	FileDescriptor retained(descriptor);
	const auto import = descriptor >= 0
							? DeviceFunction<PFN_vkImportSemaphoreFdKHR>(m_access.device, "vkImportSemaphoreFdKHR")
							: nullptr;
	VkSemaphoreCreateInfo info{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	VkSemaphore result = VK_NULL_HANDLE;
	Check(vkCreateSemaphore(m_access.device, &info, nullptr, &result), "create companion proof semaphore");
	if (descriptor >= 0)
	{
		VkImportSemaphoreFdInfoKHR imported{VK_STRUCTURE_TYPE_IMPORT_SEMAPHORE_FD_INFO_KHR};
		imported.semaphore = result;
		imported.handleType = VK_EXTERNAL_SEMAPHORE_HANDLE_TYPE_OPAQUE_FD_BIT;
		imported.fd = descriptor;
		auto status = import(m_access.device, &imported);
		if (status != VK_SUCCESS)
		{
			vkDestroySemaphore(m_access.device, result, nullptr);
			Check(status, "import companion proof semaphore");
		}
		retained.Release();
	}
	return result;
}

void CompanionFrameReceiver::Initialize()
{
	SharingPacket packet;
	sharing::Descriptors descriptors;
	sharing::Receive(m_channel, &packet, sizeof(packet), descriptors, 3);
	if (packet.version != 2 || !packet.allocationSize || packet.memoryType >= 32 ||
		packet.family != m_access.queueFamily || packet.consumerQueue != 1 ||
		(packet.imageUsage != ImageUsage && packet.imageUsage != (ImageUsage | VK_IMAGE_USAGE_COLOR_ATTACHMENT_BIT)))
		throw std::runtime_error("Invalid companion proof allocation description");
	const auto identity = Identity(m_access.physical);
	if (!std::equal(packet.deviceUuid.begin(), packet.deviceUuid.end(), identity.deviceUUID) ||
		!std::equal(packet.driverUuid.begin(), packet.driverUuid.end(), identity.driverUUID))
		throw std::runtime_error("Companion and proof producer adapter/driver UUID mismatch");
	CreateImage(m_source, packet.imageUsage, true);
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(m_access.device, m_source.image, &requirements);
	if (requirements.size != packet.allocationSize || !(requirements.memoryTypeBits & (1u << packet.memoryType)))
		throw std::runtime_error("Companion source allocation requirements do not match producer");
	Allocate(m_source, packet.allocationSize, packet.memoryType, descriptors.Take(0));
	m_ready = Semaphore(descriptors.Take(1));
	m_released = Semaphore(descriptors.Take(2));
	VkSamplerCreateInfo sampling{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampling.magFilter = sampling.minFilter = VK_FILTER_NEAREST;
	sampling.mipmapMode = VK_SAMPLER_MIPMAP_MODE_NEAREST;
	sampling.addressModeU = sampling.addressModeV = sampling.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	Check(vkCreateSampler(m_access.device, &sampling, nullptr, &m_sampler), "create retained proof image sampler");
	VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool.queueFamilyIndex = m_access.queueFamily;
	Check(vkCreateCommandPool(m_access.device, &pool, nullptr, &m_commands),
		  "create independent companion IO command pool");
	m_initialized = true;
}

VkCommandBuffer CompanionFrameReceiver::Begin()
{
	VkCommandBufferAllocateInfo allocate{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	allocate.commandPool = m_commands;
	allocate.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	allocate.commandBufferCount = 1;
	VkCommandBuffer result = VK_NULL_HANDLE;
	Check(vkAllocateCommandBuffers(m_access.device, &allocate, &result), "allocate companion IO copy commands");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	Check(vkBeginCommandBuffer(result, &begin), "begin companion IO copy");
	return result;
}

void CompanionFrameReceiver::SubmitCopy(std::uint32_t index)
{
	auto& frame = m_frames[index];
	CreateImage(frame.output, ImageUsage, false);
	VkMemoryRequirements requirements{};
	vkGetImageMemoryRequirements(m_access.device, frame.output.image, &requirements);
	Allocate(frame.output, requirements.size, MemoryType(m_access.physical, requirements.memoryTypeBits));
	VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
	view.image = frame.output.image;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D;
	view.format = ImageFormat;
	view.subresourceRange = ColorRange;
	Check(vkCreateImageView(m_access.device, &view, nullptr, &frame.output.view),
		  "create companion retained frame view");
	frame.sampleReady = Semaphore();
	VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	Check(vkCreateFence(m_access.device, &fence, nullptr, &frame.completion), "create companion copy completion");
	auto command = Begin();
	Barrier(command, m_source.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, 0, VK_ACCESS_TRANSFER_READ_BIT,
			VK_QUEUE_FAMILY_EXTERNAL, m_access.queueFamily);
	Barrier(command, frame.output.image, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
			VK_ACCESS_TRANSFER_WRITE_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
	VkImageCopy copy{};
	copy.srcSubresource = copy.dstSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.extent = ImageExtent;
	vkCmdCopyImage(command, m_source.image, VK_IMAGE_LAYOUT_GENERAL, frame.output.image, VK_IMAGE_LAYOUT_GENERAL, 1,
				   &copy);
	Barrier(command, frame.output.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_WRITE_BIT,
			VK_ACCESS_SHADER_READ_BIT | VK_ACCESS_TRANSFER_READ_BIT, VK_QUEUE_FAMILY_IGNORED, VK_QUEUE_FAMILY_IGNORED);
	Barrier(command, m_source.image, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_TRANSFER_READ_BIT, 0,
			m_access.queueFamily, VK_QUEUE_FAMILY_EXTERNAL);
	Check(vkEndCommandBuffer(command), "end companion IO copy");
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_TRANSFER_BIT;
	const std::array signals{m_released, frame.sampleReady};
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.waitSemaphoreCount = 1;
	submit.pWaitSemaphores = &m_ready;
	submit.pWaitDstStageMask = &stage;
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	submit.signalSemaphoreCount = static_cast<std::uint32_t>(signals.size());
	submit.pSignalSemaphores = signals.data();
	const auto status = vkQueueSubmit(m_access.ioQueue, 1, &submit, frame.completion);
	if (status != VK_SUCCESS)
	{
		std::fputs("FAIL: companion proof copy submission outcome is unknown\n", stderr);
		std::_Exit(1);
	}
	frame.submitted = true;
}

void CompanionFrameReceiver::Verify()
{
	constexpr VkDeviceSize bytes = ImageExtent.width * ImageExtent.height * 4;
	VkPhysicalDeviceMemoryProperties memoryProperties{};
	vkGetPhysicalDeviceMemoryProperties(m_access.physical, &memoryProperties);
	for (std::uint32_t index = 0; index < m_frames.size(); ++index)
	{
		VkBuffer buffer = VK_NULL_HANDLE;
		VkDeviceMemory memory = VK_NULL_HANDLE;
		VkFence completion = VK_NULL_HANDLE;

		struct Analysis
		{
			VkDevice device;
			VkBuffer& buffer;
			VkDeviceMemory& memory;
			VkFence& completion;
			bool pending = false;

			~Analysis()
			{
				if (pending && vkWaitForFences(device, 1, &completion, VK_TRUE, 5000000000ULL) != VK_SUCCESS)
				{
					std::fputs("FAIL: explicit proof analysis is still pending\n", stderr);
					std::_Exit(1);
				}
				if (completion)
					vkDestroyFence(device, completion, nullptr);
				if (buffer)
					vkDestroyBuffer(device, buffer, nullptr);
				if (memory)
					vkFreeMemory(device, memory, nullptr);
			}
		} analysis{m_access.device, buffer, memory, completion};

		VkBufferCreateInfo info{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
		info.size = bytes;
		info.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
		Check(vkCreateBuffer(m_access.device, &info, nullptr, &buffer),
			  "create explicitly requested proof analysis buffer");
		VkMemoryRequirements requirements{};
		vkGetBufferMemoryRequirements(m_access.device, buffer, &requirements);
		std::uint32_t type = memoryProperties.memoryTypeCount;
		for (std::uint32_t candidate = 0; candidate < memoryProperties.memoryTypeCount; ++candidate)
			if ((requirements.memoryTypeBits & (1u << candidate)) &&
				(memoryProperties.memoryTypes[candidate].propertyFlags & VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT))
			{
				type = candidate;
				break;
			}
		if (type == memoryProperties.memoryTypeCount)
			throw std::runtime_error("No host-visible explicit analysis buffer memory");
		VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocate.allocationSize = requirements.size;
		allocate.memoryTypeIndex = type;
		Check(vkAllocateMemory(m_access.device, &allocate, nullptr, &memory), "allocate explicit analysis buffer");
		Check(vkBindBufferMemory(m_access.device, buffer, memory, 0), "bind explicit analysis buffer");
		auto command = Begin();
		VkBufferImageCopy copy{};
		copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
		copy.imageExtent = ImageExtent;
		vkCmdCopyImageToBuffer(command, m_frames[index].output.image, VK_IMAGE_LAYOUT_GENERAL, buffer, 1, &copy);
		VkBufferMemoryBarrier barrier{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
		barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
		barrier.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.buffer = buffer;
		barrier.size = VK_WHOLE_SIZE;
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_HOST_BIT, 0, 0, nullptr, 1,
							 &barrier, 0, nullptr);
		Check(vkEndCommandBuffer(command), "end explicit proof analysis");
		VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
		Check(vkCreateFence(m_access.device, &fence, nullptr, &completion), "create explicit analysis completion");
		VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
		submit.commandBufferCount = 1;
		submit.pCommandBuffers = &command;
		const auto status = vkQueueSubmit(m_access.ioQueue, 1, &submit, completion);
		if (status != VK_SUCCESS)
		{
			std::fputs("FAIL: analysis queue submission outcome unknown\n", stderr);
			std::_Exit(1);
		}
		analysis.pending = true;
		Check(vkWaitForFences(m_access.device, 1, &completion, VK_TRUE, 5000000000ULL),
			  "wait explicit retained image analysis");
		analysis.pending = false;
		void* pixels = nullptr;
		Check(vkMapMemory(m_access.device, memory, 0, VK_WHOLE_SIZE, 0, &pixels),
			  "map explicit retained image analysis");

		struct Mapping
		{
			VkDevice device;
			VkDeviceMemory memory;

			~Mapping()
			{
				vkUnmapMemory(device, memory);
			}
		} mapping{m_access.device, memory};

		if (!(memoryProperties.memoryTypes[type].propertyFlags & VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
		{
			VkMappedMemoryRange range{VK_STRUCTURE_TYPE_MAPPED_MEMORY_RANGE};
			range.memory = memory;
			range.size = VK_WHOLE_SIZE;
			Check(vkInvalidateMappedMemoryRanges(m_access.device, 1, &range),
				  "invalidate explicit retained image analysis");
		}
		std::array<std::uint8_t, 4> expected{0, 0, 0, 255};
		expected[index] = 255;
		for (VkDeviceSize byte = 0; byte < bytes; ++byte)
			if (static_cast<const std::uint8_t*>(pixels)[byte] != expected[byte % 4])
				throw std::runtime_error("Companion retained image differs after source reuse at byte " +
										 std::to_string(byte));
		std::printf("Companion proof explicitAnalysis frame=%u exactRGBA=passed pixels=4096\n", index + 1);
	}
}
} // namespace simxr::app
