#include "graphics/backends/vulkan/VulkanVideoConversion.h"
#include "Pch.h"
#include "shaders/generated/HistorySpirv.h"

namespace simxr
{

static std::array<VkBufferImageCopy, 2> VideoPlanes(VkExtent2D extent)
{
	std::array<VkBufferImageCopy, 2> regions{};
	regions[0].imageSubresource = {VK_IMAGE_ASPECT_PLANE_0_BIT, 0, 0, 1};
	regions[0].imageExtent = {extent.width, extent.height, 1};
	regions[1].bufferOffset = VkDeviceSize{extent.width} * extent.height;
	regions[1].imageSubresource = {VK_IMAGE_ASPECT_PLANE_1_BIT, 0, 0, 1};
	regions[1].imageExtent = {extent.width / 2, extent.height / 2, 1};
	return regions;
}

VulkanVideoConversion::VulkanVideoConversion(std::shared_ptr<VulkanDevice> device, VkExtent2D extent)
	: m_device(std::move(device)), m_session(std::make_shared<VulkanVideoSession>(m_device, extent)),
	  m_failedOwner(std::make_unique<FailedOwner>())
{
	m_failedOwner->device = m_device;
	m_failedOwner->session = m_session;
	try
	{
		Initialize();
	}
	catch (...)
	{
		Destroy();
		throw;
	}
}

VulkanVideoConversion::~VulkanVideoConversion()
{
	Destroy();
}

VulkanH264Picture VulkanVideoConversion::Encode(std::shared_ptr<const VulkanInspectionFrame> source)
{
	if (m_uncertain)
		throw std::runtime_error("History GPU conversion completion unknown");
	if (source)
	{
		if (source->PollCompletion() != CompletionStatus::Complete || source->Image().device != m_device)
			throw std::invalid_argument("History encoder requires a completed image on its owned device");
		const auto& image = source->Image();
		if (!image.layers || image.layers > 2 || image.extent.width > Extent().width / image.layers ||
			image.extent.height > Extent().height)
			throw std::invalid_argument("History encoder dimensions mismatch");
		m_failedOwner->source = std::move(source);
		UpdateView(image.image, image.format, image.layers, true);
		Begin();
		Bind(true, image.extent, image.layers,
			 image.format == VK_FORMAT_R8G8B8A8_SRGB || image.format == VK_FORMAT_B8G8R8A8_SRGB);
	}
	else
	{
		Begin();
		const auto luma = VkDeviceSize{Extent().width} * Extent().height;
		vkCmdFillBuffer(m_command, m_buffer, 0, luma, 0);
		vkCmdFillBuffer(m_command, m_buffer, luma, luma / 2, 0x80808080);
	}
	VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	memory.srcAccessMask = VK_ACCESS_SHADER_WRITE_BIT | VK_ACCESS_TRANSFER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT | VK_PIPELINE_STAGE_TRANSFER_BIT,
						 VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 1, &memory, 0, nullptr, 0, nullptr);
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.image = m_session->Input();
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	barrier.oldLayout = m_initialized ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR : VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_DST_OPTIMAL;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
						 nullptr, 1, &barrier);
	const auto planes = VideoPlanes(Extent());
	vkCmdCopyBufferToImage(m_command, m_buffer, m_session->Input(), barrier.newLayout, planes.size(), planes.data());
	barrier.oldLayout = barrier.newLayout;
	barrier.newLayout = VK_IMAGE_LAYOUT_VIDEO_ENCODE_SRC_KHR;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	barrier.dstAccessMask = 0;
	vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
						 0, nullptr, 1, &barrier);
	Submit(VK_NULL_HANDLE, m_ready);
	// Queue encoding behind conversion on the GPU, without a CPU round trip between them.
	auto encoded = m_session->Encode(m_ready);
	Wait();
	m_initialized = true;
	vkDestroyImageView(m_device->binding.device, std::exchange(m_view, VK_NULL_HANDLE), nullptr);
	m_failedOwner->source.reset();
	return encoded;
}

std::shared_ptr<VulkanImage> VulkanVideoConversion::Decode(const VulkanH264Picture& picture, VkExtent2D extent,
														   std::uint32_t layers,
														   std::shared_ptr<FrameReservation> reservation)
{
	if (m_uncertain)
		throw std::runtime_error("History GPU conversion completion unknown");
	if (!layers || layers > 2 || extent.width > Extent().width / layers || extent.height > Extent().height)
		throw std::invalid_argument("History decoder dimensions mismatch");
	auto output =
		VulkanImage::Create(m_device, VK_FORMAT_R8G8B8A8_UNORM, extent, layers,
							VK_IMAGE_USAGE_STORAGE_BIT | VK_IMAGE_USAGE_SAMPLED_BIT | VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
							1, VK_SAMPLE_COUNT_1_BIT, VK_WHOLE_SIZE, VK_IMAGE_CREATE_MUTABLE_FORMAT_BIT);
	m_failedOwner->output = output;
	m_failedOwner->reservation = std::move(reservation);
	UpdateView(output->image, output->format, layers, false);
	m_uncertain = true;
	m_session->Decode(picture, m_decoded);
	m_uncertain = false;
	Begin();
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.image = m_session->Output();
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	barrier.oldLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.newLayout = VK_IMAGE_LAYOUT_TRANSFER_SRC_OPTIMAL;
	barrier.dstAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_TOP_OF_PIPE_BIT, VK_PIPELINE_STAGE_TRANSFER_BIT, 0, 0, nullptr, 0,
						 nullptr, 1, &barrier);
	const auto planes = VideoPlanes(Extent());
	vkCmdCopyImageToBuffer(m_command, m_session->Output(), barrier.newLayout, m_buffer, planes.size(), planes.data());
	barrier.oldLayout = barrier.newLayout;
	barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
	barrier.srcAccessMask = VK_ACCESS_TRANSFER_READ_BIT;
	barrier.dstAccessMask = 0;
	vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0, nullptr,
						 0, nullptr, 1, &barrier);
	VkMemoryBarrier memory{VK_STRUCTURE_TYPE_MEMORY_BARRIER};
	memory.srcAccessMask = VK_ACCESS_TRANSFER_WRITE_BIT;
	memory.dstAccessMask = VK_ACCESS_SHADER_READ_BIT;
	vkCmdPipelineBarrier(m_command, VK_PIPELINE_STAGE_TRANSFER_BIT, VK_PIPELINE_STAGE_COMPUTE_SHADER_BIT, 0, 1, &memory,
						 0, nullptr, 0, nullptr);
	VulkanImageBarrier(m_command, *output, VK_IMAGE_LAYOUT_UNDEFINED, VK_IMAGE_LAYOUT_GENERAL, 0,
					   VK_ACCESS_SHADER_WRITE_BIT);
	Bind(false, extent, layers, false);
	VulkanImageBarrier(m_command, *output, VK_IMAGE_LAYOUT_GENERAL, VK_IMAGE_LAYOUT_GENERAL, VK_ACCESS_SHADER_WRITE_BIT,
					   VK_ACCESS_SHADER_READ_BIT);
	Submit(m_decoded, VK_NULL_HANDLE);
	Wait();
	output->format = VK_FORMAT_R8G8B8A8_SRGB;
	vkDestroyImageView(m_device->binding.device, std::exchange(m_view, VK_NULL_HANDLE), nullptr);
	m_failedOwner->output.reset();
	m_failedOwner->reservation.reset();
	return output;
}

void VulkanVideoConversion::Initialize()
{
	const auto device = m_device->binding.device;
	const auto extent = m_session->Extent();
	VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO};
	buffer.size = VkDeviceSize{extent.width} * extent.height * 3 / 2;
	buffer.usage =
		VK_BUFFER_USAGE_STORAGE_BUFFER_BIT | VK_BUFFER_USAGE_TRANSFER_SRC_BIT | VK_BUFFER_USAGE_TRANSFER_DST_BIT;
	CheckVulkan(vkCreateBuffer(device, &buffer, nullptr, &m_buffer), "Create GPU NV12 conversion buffer");
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, m_buffer, &requirements);
	VkMemoryAllocateInfo memory{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	memory.allocationSize = requirements.size;
	memory.memoryTypeIndex = m_device->MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
	CheckVulkan(vkAllocateMemory(device, &memory, nullptr, &m_memory), "Allocate GPU NV12 conversion buffer");
	CheckVulkan(vkBindBufferMemory(device, m_buffer, m_memory, 0), "Bind GPU NV12 conversion buffer");
	VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
	pool.queueFamilyIndex = m_device->binding.queueFamily;
	pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
	CheckVulkan(vkCreateCommandPool(device, &pool, nullptr, &m_pool), "Create video conversion pool");
	VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
	command.commandPool = m_pool;
	command.commandBufferCount = 1;
	command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
	CheckVulkan(vkAllocateCommandBuffers(device, &command, &m_command), "Allocate video conversion command");
	VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CheckVulkan(vkCreateFence(device, &fence, nullptr, &m_fence), "Create video conversion fence");
	VkSemaphoreCreateInfo semaphore{VK_STRUCTURE_TYPE_SEMAPHORE_CREATE_INFO};
	CheckVulkan(vkCreateSemaphore(device, &semaphore, nullptr, &m_ready), "Create video input semaphore");
	CheckVulkan(vkCreateSemaphore(device, &semaphore, nullptr, &m_decoded), "Create video output semaphore");
	const std::array<VkDescriptorSetLayoutBinding, 3> bindings{
		{{0, VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		 {1, VK_DESCRIPTOR_TYPE_STORAGE_BUFFER, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr},
		 {2, VK_DESCRIPTOR_TYPE_STORAGE_IMAGE, 1, VK_SHADER_STAGE_COMPUTE_BIT, nullptr}}};
	VkDescriptorSetLayoutCreateInfo setLayout{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_LAYOUT_CREATE_INFO};
	setLayout.bindingCount = bindings.size();
	setLayout.pBindings = bindings.data();
	CheckVulkan(vkCreateDescriptorSetLayout(device, &setLayout, nullptr, &m_setLayout),
				"Create history shader bindings");
	std::array<VkDescriptorPoolSize, 3> sizes{};
	for (std::size_t i = 0; i < sizes.size(); ++i)
		sizes[i] = {bindings[i].descriptorType, 1};
	VkDescriptorPoolCreateInfo descriptors{VK_STRUCTURE_TYPE_DESCRIPTOR_POOL_CREATE_INFO};
	descriptors.maxSets = 1;
	descriptors.poolSizeCount = sizes.size();
	descriptors.pPoolSizes = sizes.data();
	CheckVulkan(vkCreateDescriptorPool(device, &descriptors, nullptr, &m_descriptors), "Create history descriptors");
	VkDescriptorSetAllocateInfo allocate{VK_STRUCTURE_TYPE_DESCRIPTOR_SET_ALLOCATE_INFO};
	allocate.descriptorPool = m_descriptors;
	allocate.descriptorSetCount = 1;
	allocate.pSetLayouts = &m_setLayout;
	CheckVulkan(vkAllocateDescriptorSets(device, &allocate, &m_set), "Allocate history descriptors");
	VkDescriptorBufferInfo bufferInfo{m_buffer, 0, VK_WHOLE_SIZE};
	VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	write.dstSet = m_set;
	write.dstBinding = 1;
	write.descriptorCount = 1;
	write.descriptorType = VK_DESCRIPTOR_TYPE_STORAGE_BUFFER;
	write.pBufferInfo = &bufferInfo;
	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
	VkSamplerCreateInfo sampler{VK_STRUCTURE_TYPE_SAMPLER_CREATE_INFO};
	sampler.addressModeU = sampler.addressModeV = sampler.addressModeW = VK_SAMPLER_ADDRESS_MODE_CLAMP_TO_EDGE;
	CheckVulkan(vkCreateSampler(device, &sampler, nullptr, &m_sampler), "Create history sampler");
	VkPushConstantRange constants{VK_SHADER_STAGE_COMPUTE_BIT, 0, 6 * sizeof(std::uint32_t)};
	VkPipelineLayoutCreateInfo layout{VK_STRUCTURE_TYPE_PIPELINE_LAYOUT_CREATE_INFO};
	layout.setLayoutCount = 1;
	layout.pSetLayouts = &m_setLayout;
	layout.pushConstantRangeCount = 1;
	layout.pPushConstantRanges = &constants;
	CheckVulkan(vkCreatePipelineLayout(device, &layout, nullptr, &m_layout), "Create history pipeline layout");
	for (bool encode : {true, false})
	{
		const auto words = encode ? std::span<const std::uint32_t>(shader::HistoryEncode)
								  : std::span<const std::uint32_t>(shader::HistoryDecode);
		VkShaderModuleCreateInfo moduleInfo{VK_STRUCTURE_TYPE_SHADER_MODULE_CREATE_INFO};
		moduleInfo.codeSize = words.size_bytes();
		moduleInfo.pCode = words.data();
		VkShaderModule module;
		CheckVulkan(vkCreateShaderModule(device, &moduleInfo, nullptr, &module), "Create history compute shader");
		VkComputePipelineCreateInfo pipeline{VK_STRUCTURE_TYPE_COMPUTE_PIPELINE_CREATE_INFO};
		pipeline.layout = m_layout;
		pipeline.stage = {VK_STRUCTURE_TYPE_PIPELINE_SHADER_STAGE_CREATE_INFO};
		pipeline.stage.stage = VK_SHADER_STAGE_COMPUTE_BIT;
		pipeline.stage.module = module;
		pipeline.stage.pName = "main";
		const auto result =
			vkCreateComputePipelines(device, VK_NULL_HANDLE, 1, &pipeline, nullptr, encode ? &m_encode : &m_decode);
		vkDestroyShaderModule(device, module, nullptr);
		CheckVulkan(result, "Create history conversion pipeline");
	}
}

void VulkanVideoConversion::Destroy() noexcept
{
	if (m_uncertain)
	{
		m_failedOwner->preservation.Preserve(std::move(m_failedOwner));
		return;
	}
	const auto device = m_device->binding.device;
	if (m_view)
		vkDestroyImageView(device, m_view, nullptr);
	if (m_encode)
		vkDestroyPipeline(device, m_encode, nullptr);
	if (m_decode)
		vkDestroyPipeline(device, m_decode, nullptr);
	if (m_layout)
		vkDestroyPipelineLayout(device, m_layout, nullptr);
	if (m_descriptors)
		vkDestroyDescriptorPool(device, m_descriptors, nullptr);
	if (m_setLayout)
		vkDestroyDescriptorSetLayout(device, m_setLayout, nullptr);
	if (m_sampler)
		vkDestroySampler(device, m_sampler, nullptr);
	if (m_pool)
		vkDestroyCommandPool(device, m_pool, nullptr);
	if (m_fence)
		vkDestroyFence(device, m_fence, nullptr);
	if (m_ready)
		vkDestroySemaphore(device, m_ready, nullptr);
	if (m_decoded)
		vkDestroySemaphore(device, m_decoded, nullptr);
	if (m_buffer)
		vkDestroyBuffer(device, m_buffer, nullptr);
	if (m_memory)
		vkFreeMemory(device, m_memory, nullptr);
}

void VulkanVideoConversion::Begin()
{
	if (m_uncertain)
		throw std::runtime_error("History GPU conversion completion unknown");
	CheckVulkan(vkResetCommandBuffer(m_command, 0), "Reset video conversion command");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CheckVulkan(vkBeginCommandBuffer(m_command, &begin), "Begin video conversion command");
}

void VulkanVideoConversion::Submit(VkSemaphore wait, VkSemaphore signal)
{
	const auto device = m_device->binding.device;
	CheckVulkan(vkEndCommandBuffer(m_command), "End video conversion command");
	CheckVulkan(vkResetFences(device, 1, &m_fence), "Reset video conversion fence");
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &m_command;
	submit.waitSemaphoreCount = wait ? 1 : 0;
	submit.pWaitSemaphores = &wait;
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	submit.pWaitDstStageMask = &stage;
	submit.signalSemaphoreCount = signal ? 1 : 0;
	submit.pSignalSemaphores = &signal;
	{
		std::lock_guard lock(m_device->queueMutex);
		m_uncertain = true;
		CheckVulkan(vkQueueSubmit(m_device->queue, 1, &submit, m_fence), "Submit GPU video conversion");
	}
}

void VulkanVideoConversion::Wait()
{
	CheckVulkan(vkWaitForFences(m_device->binding.device, 1, &m_fence, VK_TRUE, 5000000000ULL),
				"Wait for GPU video conversion");
	m_uncertain = false;
}

void VulkanVideoConversion::Bind(bool encode, VkExtent2D extent, std::uint32_t layers, bool srgb)
{
	const auto coded = m_session->Extent();
	const std::array<std::uint32_t, 6> shape{extent.width,		   extent.height, layers,
											 coded.width / layers, coded.height,  srgb ? 1u : 0u};
	vkCmdBindPipeline(m_command, VK_PIPELINE_BIND_POINT_COMPUTE, encode ? m_encode : m_decode);
	vkCmdBindDescriptorSets(m_command, VK_PIPELINE_BIND_POINT_COMPUTE, m_layout, 0, 1, &m_set, 0, nullptr);
	vkCmdPushConstants(m_command, m_layout, VK_SHADER_STAGE_COMPUTE_BIT, 0, sizeof(shape), shape.data());
	vkCmdDispatch(m_command, encode ? (coded.width + 31) / 32 : (extent.width + 7) / 8,
				  encode ? (coded.height + 7) / 8 : (extent.height + 7) / 8, encode ? 1 : layers);
}

void VulkanVideoConversion::UpdateView(VkImage image, VkFormat format, std::uint32_t layers, bool encode)
{
	const auto device = m_device->binding.device;
	if (m_view)
		vkDestroyImageView(device, std::exchange(m_view, VK_NULL_HANDLE), nullptr);
	VkImageViewUsageCreateInfo usage{VK_STRUCTURE_TYPE_IMAGE_VIEW_USAGE_CREATE_INFO};
	usage.usage = encode ? VK_IMAGE_USAGE_SAMPLED_BIT : VK_IMAGE_USAGE_STORAGE_BIT;
	VkImageViewCreateInfo view{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO, &usage};
	view.image = image;
	view.format = format;
	view.viewType = VK_IMAGE_VIEW_TYPE_2D_ARRAY;
	view.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, layers};
	CheckVulkan(vkCreateImageView(device, &view, nullptr, &m_view), "Create history conversion view");
	VkDescriptorImageInfo imageInfo{encode ? m_sampler : VK_NULL_HANDLE, m_view, VK_IMAGE_LAYOUT_GENERAL};
	VkWriteDescriptorSet write{VK_STRUCTURE_TYPE_WRITE_DESCRIPTOR_SET};
	write.dstSet = m_set;
	write.dstBinding = encode ? 0 : 2;
	write.descriptorCount = 1;
	write.descriptorType = encode ? VK_DESCRIPTOR_TYPE_COMBINED_IMAGE_SAMPLER : VK_DESCRIPTOR_TYPE_STORAGE_IMAGE;
	write.pImageInfo = &imageInfo;
	vkUpdateDescriptorSets(device, 1, &write, 0, nullptr);
}
} // namespace simxr
