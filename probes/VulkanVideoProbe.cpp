#include "Pch.h"
#include "VulkanProbeInstance.h"
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanInspectionDevice.h"
#include "graphics/backends/vulkan/VulkanVideoSession.h"
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>

namespace simxr::probe
{
static void Transfer(const std::shared_ptr<VulkanDevice>& owner, VulkanVideoSession& video, bool upload,
					 bool initialized, std::uint8_t value)
{
	const auto device = owner->binding.device;
	const auto extent = video.Extent();
	const auto pixels = extent.width * extent.height;
	VkBufferCreateInfo bufferInfo{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	bufferInfo.size = pixels * 3 / 2;
	bufferInfo.usage = VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	VkBuffer buffer;
	CheckVulkan(vkCreateBuffer(device, &bufferInfo, nullptr, &buffer), "Create test NV12 buffer");
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, buffer, &requirements);
	VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = owner->MemoryType(
		requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	VkDeviceMemory memory;
	CheckVulkan(vkAllocateMemory(device, &allocation, nullptr, &memory), "Allocate test NV12 buffer");
	CheckVulkan(vkBindBufferMemory(device, buffer, memory, 0), "Bind test NV12 buffer");
	void* mapped;
	CheckVulkan(vkMapMemory(device, memory, 0, VK_WHOLE_SIZE, 0, &mapped), "Map test NV12 buffer");
	if (upload)
	{
		std::memset(mapped, value, pixels);
		std::memset(static_cast<std::uint8_t*>(mapped) + pixels, 128, pixels / 2);
	}
	VkCommandPoolCreateInfo poolInfo{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	poolInfo.queueFamilyIndex = upload ? owner->encodeFamily : owner->decodeFamily;
	VkCommandPool pool;
	CheckVulkan(vkCreateCommandPool(device, &poolInfo, nullptr, &pool), "Create video test pool");
	VkCommandBufferAllocateInfo commandInfo{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	commandInfo.commandPool = pool;
	commandInfo.commandBufferCount = 1;
	commandInfo.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	VkCommandBuffer command;
	CheckVulkan(vkAllocateCommandBuffers(device, &commandInfo, &command), "Allocate video test command");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	CheckVulkan(vkBeginCommandBuffer(command, &begin), "Begin video test command");
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.image = upload ? video.Input() : video.Output();
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	barrier.oldLayout = upload ? (initialized ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED)
							   : VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = upload ? VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL : VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.srcAccessMask = initialized || !upload ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0;
	barrier.dstAccessMask = upload ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
						 nullptr, 1, &barrier);
	std::array<VkBufferImageCopy, 2> regions{};
	regions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
	regions[0].imageExtent = {extent.width, extent.height, 1};
	regions[1].bufferOffset = pixels;
	regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
	regions[1].imageExtent = {extent.width / 2, extent.height / 2, 1};
	if (upload)
		vkCmdCopyBufferToImage(command, buffer, video.Input(), barrier.newLayout, 2, regions.data());
	else
		vkCmdCopyImageToBuffer(command, video.Output(), barrier.newLayout, buffer, 2, regions.data());
	barrier.oldLayout = barrier.newLayout;
	barrier.newLayout = upload ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR : VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcAccessMask = upload ? VK_ACCESS_TRANSFER_WRITE_BIT : VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr, 0,
						 nullptr, 1, &barrier);
	CheckVulkan(vkEndCommandBuffer(command), "End video test command");
	VkQueue queue;
	vkGetDeviceQueue(device, poolInfo.queueFamilyIndex, 0, &queue);
	VkFenceCreateInfo fenceInfo{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	VkFence fence;
	CheckVulkan(vkCreateFence(device, &fenceInfo, nullptr, &fence), "Create video test fence");
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	if (vkQueueSubmit(queue, 1, &submit, fence) != VK_SUCCESS ||
		vkWaitForFences(device, 1, &fence, VK_TRUE, 5000000000ULL) != VK_SUCCESS)
	{
		std::fputs("FAIL: video test completion unknown; retaining native resources\n", stderr);
		std::_Exit(1);
	}
	bool valid = true;
	if (!upload)
		for (std::uint32_t i = 0; i < pixels * 3 / 2; ++i)
			valid &= std::abs(int(static_cast<const std::uint8_t*>(mapped)[i]) - (i < pixels ? value : 128)) <= 3;
	vkDestroyFence(device, fence, nullptr);
	vkDestroyCommandPool(device, pool, nullptr);
	vkUnmapMemory(device, memory);
	vkDestroyBuffer(device, buffer, nullptr);
	vkFreeMemory(device, memory, nullptr);
	if (!valid)
		throw std::runtime_error("Decoded H.264 pixels mismatch");
}
} // namespace simxr::probe

int main()
{
	try
	{
		simxr::probe::Instance instance;
		instance.Create(VK_API_VERSION_1_3);
		std::uint32_t count = 0;
		simxr::CheckVulkan(vkEnumeratePhysicalDevices(instance.handle, &count, nullptr), "Count devices");
		std::vector<VkPhysicalDevice> devices(count);
		simxr::CheckVulkan(vkEnumeratePhysicalDevices(instance.handle, &count, devices.data()), "Enumerate devices");
		for (auto physical : devices)
		{
			VkPhysicalDeviceIDProperties ids{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_ID_PROPERTIES};
			VkPhysicalDeviceProperties2 properties{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_PROPERTIES_2, &ids};
			vkGetPhysicalDeviceProperties2(physical, &properties);
			if (properties.properties.deviceType != VK_PHYSICAL_DEVICE_TYPE_DISCRETE_GPU)
				continue;
			simxr::VulkanExternalIdentity identity;
			std::copy_n(ids.deviceUUID, VK_UUID_SIZE, identity.deviceUuid.begin());
			std::copy_n(ids.driverUUID, VK_UUID_SIZE, identity.driverUuid.begin());
			auto validation = std::make_shared<simxr::VulkanInspectionValidation>();
			{
				simxr::VulkanInspectionDevice owner(identity, validation);
				simxr::VulkanVideoSession video(owner.Device(), {640, 480});
				std::vector<simxr::VulkanH264Picture> frames;
				for (unsigned i = 0; i < 8; ++i)
				{
					simxr::probe::Transfer(owner.Device(), video, true, i != 0, 32 + i * 24);
					frames.push_back(video.Encode());
					std::printf("Encoded frame %u: %zu bytes\n", i, frames.back().bytes.size());
				}
				for (unsigned i : {7u, 0u, 3u, 1u, 6u})
				{
					video.Decode(frames[i]);
					simxr::probe::Transfer(owner.Device(), video, false, true, 32 + i * 24);
				}
			}
			if (validation->errors.load() || simxr::probe::s_validationErrors.load())
				throw std::runtime_error("Vulkan validation failed");
			std::puts("H.264 GPU encode/decode and out-of-order pixel checks passed");
			return 0;
		}
		throw std::runtime_error("No discrete video test device");
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
