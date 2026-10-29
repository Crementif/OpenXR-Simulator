#include "app/UiPch.h"
#include "app/render/UiRenderResources.h"
#include "app/render/UiRenderer.h"
#include "app/services/Utf8Path.h"

namespace simxr::app
{
void UiRenderer::PrepareReadback()
{
	auto& gpu = *m_resources;
	if (gpu.format != VK_FORMAT_B8G8R8A8_UNORM && gpu.format != VK_FORMAT_B8G8R8A8_SRGB &&
		gpu.format != VK_FORMAT_R8G8B8A8_UNORM && gpu.format != VK_FORMAT_R8G8B8A8_SRGB)
		throw std::runtime_error("Explicit screenshot requires an 8-bit RGBA/BGRA surface format");
	const auto width = gpu.extent.width, height = gpu.extent.height;
	const auto pixels = std::uint64_t(width) * height;
	if (!width || !height || width > INT32_MAX || height > INT32_MAX ||
		pixels > (std::numeric_limits<std::uint32_t>::max() - 54) / 4 ||
		pixels > std::numeric_limits<std::size_t>::max() / 4 ||
		pixels > static_cast<std::uint64_t>(std::numeric_limits<std::streamsize>::max()) / 4)
		throw std::length_error("Screenshot dimensions exceed BMP or host buffer limits");
	vkDestroyBuffer(gpu.device, gpu.readback, nullptr);
	gpu.readback = VK_NULL_HANDLE;
	vkFreeMemory(gpu.device, gpu.readbackMemory, nullptr);
	gpu.readbackMemory = VK_NULL_HANDLE;
	VkBufferCreateInfo create{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	create.size = pixels * 4;
	create.usage = VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	CheckResult(vkCreateBuffer(gpu.device, &create, nullptr, &gpu.readback));
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(gpu.device, gpu.readback, &requirements);
	VkPhysicalDeviceMemoryProperties properties;
	vkGetPhysicalDeviceMemoryProperties(gpu.physical, &properties);
	std::uint32_t type = properties.memoryTypeCount;
	for (std::uint32_t index = 0; index < properties.memoryTypeCount; ++index)
		if ((requirements.memoryTypeBits & (1u << index)) &&
			(properties.memoryTypes[index].propertyFlags &
			 (VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT)) ==
				(VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT))
		{
			type = index;
			break;
		}
	if (type == properties.memoryTypeCount)
		throw std::runtime_error("Screenshot has no coherent host memory type");
	VkMemoryAllocateInfo allocate{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocate.allocationSize = requirements.size;
	allocate.memoryTypeIndex = type;
	CheckResult(vkAllocateMemory(gpu.device, &allocate, nullptr, &gpu.readbackMemory));
	CheckResult(vkBindBufferMemory(gpu.device, gpu.readback, gpu.readbackMemory, 0));
}

void UiRenderer::RecordReadback(VkCommandBuffer command, VkImage source)
{
	auto& gpu = *m_resources;
	VkImageMemoryBarrier image{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	image.srcAccessMask = VK_ACCESS_COLOR_ATTACHMENT_WRITE_BIT;
	image.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	image.oldLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	image.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	image.srcQueueFamilyIndex = image.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	image.image = source;
	image.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_COLOR_ATTACHMENT_OUTPUT_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0,
						 nullptr, 0, nullptr, 1, &image);
	VkBufferImageCopy copy{};
	copy.imageSubresource = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 0, 1};
	copy.imageExtent = {gpu.extent.width, gpu.extent.height, 1};
	vkCmdCopyImageToBuffer(command, image.image, image.newLayout, gpu.readback, 1, &copy);
	image.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	image.dstAccessMask = 0;
	image.oldLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	image.newLayout = VK_IMAGE_LAYOUT_PRESENT_SRC_KHR;
	VkBufferMemoryBarrier buffer{VK_STRUCTURE_TYPE_BUFFER_MEMORY_BARRIER};
	buffer.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	buffer.dstAccessMask = VK_ACCESS_HOST_READ_BIT;
	buffer.srcQueueFamilyIndex = buffer.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	buffer.buffer = gpu.readback;
	buffer.size = VK_WHOLE_SIZE;
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_TRANSFER_BIT,
						 VK_PIPELINE_STAGE_HOST_BIT | VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr, 1, &buffer,
						 1, &image);
}

void UiRenderer::WriteScreenshot()
{
	auto& gpu = *m_resources;
	const auto byteCount = static_cast<std::size_t>(gpu.extent.width) * gpu.extent.height * 4;
	std::vector<unsigned char> pixels(byteCount);
	void* mapped = nullptr;
	CheckResult(vkMapMemory(gpu.device, gpu.readbackMemory, 0, byteCount, 0, &mapped));
	std::memcpy(pixels.data(), mapped, byteCount);
	vkUnmapMemory(gpu.device, gpu.readbackMemory);
	if (gpu.format == VK_FORMAT_R8G8B8A8_UNORM || gpu.format == VK_FORMAT_R8G8B8A8_SRGB)
		for (std::size_t index = 0; index < pixels.size(); index += 4)
			std::swap(pixels[index], pixels[index + 2]);
	std::array<unsigned char, 54> header{};
	header[0] = 'B';
	header[1] = 'M';
	auto write = [&](std::size_t offset, std::uint32_t value) {
		for (unsigned index = 0; index < 4; ++index)
			header[offset + index] = static_cast<unsigned char>(value >> (8 * index));
	};
	write(2, static_cast<std::uint32_t>(54 + byteCount));
	write(10, 54);
	write(14, 40);
	write(18, gpu.extent.width);
	write(22, 0u - gpu.extent.height);
	header[26] = 1;
	header[28] = 32;
	std::ofstream file(m_screenshotPath, std::ios::binary);
	file.exceptions(std::ios::failbit | std::ios::badbit);
	file.write(reinterpret_cast<const char*>(header.data()), header.size());
	file.write(reinterpret_cast<const char*>(pixels.data()), static_cast<std::streamsize>(pixels.size()));
	file.close();
	std::printf("Explicit UI screenshot: %s (%u x %u)\n", PathText(m_screenshotPath).c_str(), gpu.extent.width,
				gpu.extent.height);
	m_screenshotPath.clear();
}

} // namespace simxr::app
