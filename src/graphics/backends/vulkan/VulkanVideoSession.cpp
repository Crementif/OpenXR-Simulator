#include "graphics/backends/vulkan/VulkanVideoSession.h"
#include "Pch.h"

namespace simxr
{
VulkanVideoSession::VulkanVideoSession(std::shared_ptr<VulkanDevice> device, VkExtent2D extent)
	: m_device(std::move(device)), m_extent(extent)
{
	m_failedOwner = std::make_unique<FailedOwner>();
	m_failedOwner->device = m_device;
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

VulkanVideoSession::~VulkanVideoSession()
{
	Destroy();
}

void VulkanVideoSession::Destroy() noexcept
{
	if (m_uncertain)
	{
		m_failedOwner->preservation.Preserve(std::move(m_failedOwner));
		return;
	}
	const auto device = m_device->binding.device;
	if (m_encodePool)
		vkDestroyCommandPool(device, m_encodePool, nullptr);
	if (m_decodePool)
		vkDestroyCommandPool(device, m_decodePool, nullptr);
	if (m_fence)
		vkDestroyFence(device, m_fence, nullptr);
	if (m_query)
		vkDestroyQueryPool(device, m_query, nullptr);
	if (m_decodeQuery)
		vkDestroyQueryPool(device, m_decodeQuery, nullptr);
	if (m_encodeParameters)
		m_destroyParameters(device, m_encodeParameters, nullptr);
	if (m_decodeParameters)
		m_destroyParameters(device, m_decodeParameters, nullptr);
	if (m_encode)
		m_destroySession(device, m_encode, nullptr);
	if (m_decode)
		m_destroySession(device, m_decode, nullptr);
	for (auto view : {m_inputView, m_dpbView, m_outputView})
		if (view)
			vkDestroyImageView(device, view, nullptr);
	for (auto image : {m_input, m_dpb, m_output})
		if (image)
			vkDestroyImage(device, image, nullptr);
	for (auto memory : m_memory)
		vkFreeMemory(device, memory, nullptr);
	if (m_mapped)
		vkUnmapMemory(device, m_bitstreamMemory);
	if (m_bitstream)
		vkDestroyBuffer(device, m_bitstream, nullptr);
	if (m_bitstreamMemory)
		vkFreeMemory(device, m_bitstreamMemory, nullptr);
}

void VulkanVideoSession::Initialize()
{
	if (m_device->encodeFamily == VK_QUEUE_FAMILY_IGNORED || m_device->decodeFamily == VK_QUEUE_FAMILY_IGNORED)
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Vulkan H.264 encode/decode unavailable");
	if (!m_extent.width || !m_extent.height || m_extent.width % 16 || m_extent.height % 16)
		throw std::invalid_argument("H.264 history dimensions must be multiples of 16");
	const auto device = m_device->binding.device;
#define VIDEO_COMMAND(member, name)                                                                                    \
	member = reinterpret_cast<PFN_##name>(vkGetDeviceProcAddr(device, #name));                                         \
	if (!member)                                                                                                       \
	throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, #name)
	VIDEO_COMMAND(m_createSession, vkCreateVideoSessionKHR);
	VIDEO_COMMAND(m_destroySession, vkDestroyVideoSessionKHR);
	VIDEO_COMMAND(m_getMemory, vkGetVideoSessionMemoryRequirementsKHR);
	VIDEO_COMMAND(m_bindMemory, vkBindVideoSessionMemoryKHR);
	VIDEO_COMMAND(m_createParameters, vkCreateVideoSessionParametersKHR);
	VIDEO_COMMAND(m_destroyParameters, vkDestroyVideoSessionParametersKHR);
	VIDEO_COMMAND(m_getParameters, vkGetEncodedVideoSessionParametersKHR);
	VIDEO_COMMAND(m_begin, vkCmdBeginVideoCodingKHR);
	VIDEO_COMMAND(m_end, vkCmdEndVideoCodingKHR);
	VIDEO_COMMAND(m_control, vkCmdControlVideoCodingKHR);
	VIDEO_COMMAND(m_encodeVideo, vkCmdEncodeVideoKHR);
	VIDEO_COMMAND(m_decodeVideo, vkCmdDecodeVideoKHR);
#undef VIDEO_COMMAND
	m_encodeProfile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
	m_decodeProfile.stdProfileIdc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
	m_decodeProfile.pictureLayout = VK_VIDEO_DECODE_H264_PICTURE_LAYOUT_PROGRESSIVE_KHR;
	for (auto& profile : m_profiles)
	{
		profile.sType = VK_STRUCTURE_TYPE_VIDEO_PROFILE_INFO_KHR;
		profile.chromaSubsampling = VK_VIDEO_CHROMA_SUBSAMPLING_420_BIT_KHR;
		profile.lumaBitDepth = profile.chromaBitDepth = VK_VIDEO_COMPONENT_BIT_DEPTH_8_BIT_KHR;
	}
	m_profiles[0].pNext = &m_encodeProfile;
	m_profiles[0].videoCodecOperation = VK_VIDEO_CODEC_OPERATION_ENCODE_H264_BIT_KHR;
	m_profiles[1].pNext = &m_decodeProfile;
	m_profiles[1].videoCodecOperation = VK_VIDEO_CODEC_OPERATION_DECODE_H264_BIT_KHR;
	auto capabilities = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoCapabilitiesKHR>(
		vkGetInstanceProcAddr(m_device->binding.instance, "vkGetPhysicalDeviceVideoCapabilitiesKHR"));
	if (!capabilities)
		throw VulkanError(VK_ERROR_EXTENSION_NOT_PRESENT, "Vulkan video capabilities unavailable");
	VkVideoEncodeCapabilitiesKHR encodeCaps{VK_STRUCTURE_TYPE_VIDEO_ENCODE_CAPABILITIES_KHR, &m_h264Caps};
	m_encodeCaps.pNext = &encodeCaps;
	CheckVulkan(capabilities(m_device->binding.physicalDevice, &m_profiles[0], &m_encodeCaps), "Query H.264 encoding");
	m_encodeCaps.pNext = nullptr;
	VkVideoDecodeH264CapabilitiesKHR h264Decode{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_CAPABILITIES_KHR};
	VkVideoDecodeCapabilitiesKHR decodeCaps{VK_STRUCTURE_TYPE_VIDEO_DECODE_CAPABILITIES_KHR, &h264Decode};
	m_decodeCaps.pNext = &decodeCaps;
	CheckVulkan(capabilities(m_device->binding.physicalDevice, &m_profiles[1], &m_decodeCaps), "Query H.264 decoding");
	m_decodeCaps.pNext = nullptr;
	if (!(encodeCaps.rateControlModes & VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR) ||
		!(decodeCaps.flags & VK_VIDEO_DECODE_CAPABILITY_DPB_AND_OUTPUT_COINCIDE_BIT_KHR))
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "Independent H.264 pictures unsupported");
	for (const auto& caps : {m_encodeCaps, m_decodeCaps})
		if (m_extent.width < caps.minCodedExtent.width || m_extent.height < caps.minCodedExtent.height ||
			m_extent.width > caps.maxCodedExtent.width || m_extent.height > caps.maxCodedExtent.height)
			throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED, "H.264 extent unsupported");
	m_vui.flags.video_signal_type_present_flag = m_vui.flags.color_description_present_flag = 1;
	m_vui.flags.video_full_range_flag = 1;
	m_vui.video_format = 5;
	m_vui.colour_primaries = m_vui.matrix_coefficients = 1;
	m_vui.transfer_characteristics = 13;
	m_sps.flags.vui_parameters_present_flag = 1;
	m_sps.pSequenceParameterSetVui = &m_vui;
	m_sps.profile_idc = STD_VIDEO_H264_PROFILE_IDC_MAIN;
	m_sps.level_idc = STD_VIDEO_H264_LEVEL_IDC_4_1;
	m_sps.chroma_format_idc = STD_VIDEO_H264_CHROMA_FORMAT_IDC_420;
	m_sps.flags.frame_mbs_only_flag = 1;
	m_sps.flags.direct_8x8_inference_flag = 1;
	m_sps.pic_order_cnt_type = STD_VIDEO_H264_POC_TYPE_0;
	m_sps.max_num_ref_frames = 1;
	m_sps.pic_width_in_mbs_minus1 = m_extent.width / 16 - 1;
	m_sps.pic_height_in_map_units_minus1 = m_extent.height / 16 - 1;
	m_pps.flags.entropy_coding_mode_flag = 1;
	m_pps.flags.deblocking_filter_control_present_flag = 1;
	CreateSession(true);
	CreateSession(false);
	m_input = CreateImage(VK_IMAGE_USAGE_VIDEO_ENCODE_SRC_BIT_KHR | VK_IMAGE_USAGE_TRANSFER_DST_BIT, true, m_inputView);
	m_dpb = CreateImage(VK_IMAGE_USAGE_VIDEO_ENCODE_DPB_BIT_KHR, true, m_dpbView);
	m_output = CreateImage(VK_IMAGE_USAGE_VIDEO_DECODE_DST_BIT_KHR | VK_IMAGE_USAGE_VIDEO_DECODE_DPB_BIT_KHR |
							   VK_IMAGE_USAGE_TRANSFER_SRC_BIT,
						   false, m_outputView);
	VkVideoEncodeH264SessionParametersGetInfoKHR h264Get{
		VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_GET_INFO_KHR};
	h264Get.writeStdSPS = h264Get.writeStdPPS = VK_TRUE;
	VkVideoEncodeSessionParametersGetInfoKHR get{VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_GET_INFO_KHR,
												 &h264Get};
	get.videoSessionParameters = m_encodeParameters;
	VkVideoEncodeSessionParametersFeedbackInfoKHR feedback{
		VK_STRUCTURE_TYPE_VIDEO_ENCODE_SESSION_PARAMETERS_FEEDBACK_INFO_KHR};
	std::size_t size = 0;
	CheckVulkan(m_getParameters(device, &get, &feedback, &size, nullptr), "Size encoded H.264 headers");
	m_headers.resize(size);
	CheckVulkan(m_getParameters(device, &get, &feedback, &size, m_headers.data()), "Read encoded H.264 headers");
	m_headers.resize(size);
	// Driver overrides require parsing the returned SPS/PPS before decoding.
	if (feedback.hasOverrides)
		throw VulkanError(VK_ERROR_FEATURE_NOT_PRESENT, "H.264 driver parameter overrides require parsing");
	VkVideoProfileListInfoKHR profiles{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
	profiles.profileCount = 2;
	profiles.pProfiles = m_profiles;
	VkBufferCreateInfo buffer{VK_STRUCTURE_TYPE_BUFFER_CREATE_INFO, &profiles};
	m_bitstreamSize = 4ull << 20;
	buffer.size = m_bitstreamSize;
	buffer.usage = VK_BUFFER_USAGE_VIDEO_ENCODE_DST_BIT_KHR | VK_BUFFER_USAGE_VIDEO_DECODE_SRC_BIT_KHR;
	CheckVulkan(vkCreateBuffer(device, &buffer, nullptr, &m_bitstream), "Create H.264 bitstream buffer");
	VkMemoryRequirements requirements;
	vkGetBufferMemoryRequirements(device, m_bitstream, &requirements);
	VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
	allocation.allocationSize = requirements.size;
	allocation.memoryTypeIndex = m_device->MemoryType(
		requirements.memoryTypeBits, VK_MEMORY_PROPERTY_HOST_VISIBLE_BIT | VK_MEMORY_PROPERTY_HOST_COHERENT_BIT);
	CheckVulkan(vkAllocateMemory(device, &allocation, nullptr, &m_bitstreamMemory), "Allocate H.264 bitstream memory");
	CheckVulkan(vkBindBufferMemory(device, m_bitstream, m_bitstreamMemory, 0), "Bind H.264 bitstream memory");
	CheckVulkan(vkMapMemory(device, m_bitstreamMemory, 0, VK_WHOLE_SIZE, 0, &m_mapped), "Map compressed H.264 data");
	VkQueryPoolVideoEncodeFeedbackCreateInfoKHR queryFeedback{
		VK_STRUCTURE_TYPE_QUERY_POOL_VIDEO_ENCODE_FEEDBACK_CREATE_INFO_KHR};
	queryFeedback.encodeFeedbackFlags = VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BUFFER_OFFSET_BIT_KHR |
										VK_VIDEO_ENCODE_FEEDBACK_BITSTREAM_BYTES_WRITTEN_BIT_KHR;
	queryFeedback.pNext = &m_profiles[0];
	VkQueryPoolCreateInfo query{VK_STRUCTURE_TYPE_QUERY_POOL_CREATE_INFO, &queryFeedback};
	query.queryType = VK_QUERY_TYPE_VIDEO_ENCODE_FEEDBACK_KHR;
	query.queryCount = 1;
	CheckVulkan(vkCreateQueryPool(device, &query, nullptr, &m_query), "Create H.264 feedback query");
	query.pNext = &m_profiles[1];
	query.queryType = VK_QUERY_TYPE_RESULT_STATUS_ONLY_KHR;
	CheckVulkan(vkCreateQueryPool(device, &query, nullptr, &m_decodeQuery), "Create H.264 decode status query");
	VkFenceCreateInfo fence{VK_STRUCTURE_TYPE_FENCE_CREATE_INFO};
	CheckVulkan(vkCreateFence(device, &fence, nullptr, &m_fence), "Create video completion fence");
	for (bool encode : {true, false})
	{
		VkCommandPoolCreateInfo pool{VK_STRUCTURE_TYPE_COMMAND_POOL_CREATE_INFO};
		pool.queueFamilyIndex = encode ? m_device->encodeFamily : m_device->decodeFamily;
		pool.flags = VK_COMMAND_POOL_CREATE_RESET_COMMAND_BUFFER_BIT;
		auto& handle = encode ? m_encodePool : m_decodePool;
		CheckVulkan(vkCreateCommandPool(device, &pool, nullptr, &handle), "Create video command pool");
		VkCommandBufferAllocateInfo command{VK_STRUCTURE_TYPE_COMMAND_BUFFER_ALLOCATE_INFO};
		command.commandPool = handle;
		command.level = VK_COMMAND_BUFFER_LEVEL_PRIMARY;
		command.commandBufferCount = 1;
		CheckVulkan(vkAllocateCommandBuffers(device, &command, encode ? &m_encodeCommand : &m_decodeCommand),
					"Allocate video command");
	}
}

void VulkanVideoSession::CreateSession(bool encode)
{
	const auto device = m_device->binding.device;
	const auto& caps = encode ? m_encodeCaps : m_decodeCaps;
	VkVideoSessionCreateInfoKHR create{VK_STRUCTURE_TYPE_VIDEO_SESSION_CREATE_INFO_KHR};
	create.queueFamilyIndex = encode ? m_device->encodeFamily : m_device->decodeFamily;
	create.pVideoProfile = &m_profiles[encode ? 0 : 1];
	create.pictureFormat = create.referencePictureFormat = VK_FORMAT_G8_B8R8_2PLANE_420_UNORM;
	create.maxCodedExtent = m_extent;
	create.maxDpbSlots = 1;
	create.maxActiveReferencePictures = 1;
	create.pStdHeaderVersion = &caps.stdHeaderVersion;
	auto& session = encode ? m_encode : m_decode;
	CheckVulkan(m_createSession(device, &create, nullptr, &session), "Create H.264 session");
	std::uint32_t count = 0;
	CheckVulkan(m_getMemory(device, session, &count, nullptr), "Query H.264 memory");
	std::vector<VkVideoSessionMemoryRequirementsKHR> requirements(
		count, {VK_STRUCTURE_TYPE_VIDEO_SESSION_MEMORY_REQUIREMENTS_KHR});
	CheckVulkan(m_getMemory(device, session, &count, requirements.data()), "Read H.264 memory requirements");
	m_memory.reserve(m_memory.size() + requirements.size());
	for (const auto& requirement : requirements)
	{
		VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO};
		allocation.allocationSize = requirement.memoryRequirements.size;
		allocation.memoryTypeIndex = m_device->MemoryType(requirement.memoryRequirements.memoryTypeBits, 0);
		VkDeviceMemory memory;
		CheckVulkan(vkAllocateMemory(device, &allocation, nullptr, &memory), "Allocate H.264 session memory");
		m_memory.push_back(memory);
		VkBindVideoSessionMemoryInfoKHR bind{VK_STRUCTURE_TYPE_BIND_VIDEO_SESSION_MEMORY_INFO_KHR};
		bind.memoryBindIndex = requirement.memoryBindIndex;
		bind.memory = memory;
		bind.memorySize = allocation.allocationSize;
		CheckVulkan(m_bindMemory(device, session, 1, &bind), "Bind H.264 session memory");
	}
	VkVideoEncodeH264SessionParametersAddInfoKHR encodeAdd{
		VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
	encodeAdd.stdSPSCount = encodeAdd.stdPPSCount = 1;
	encodeAdd.pStdSPSs = &m_sps;
	encodeAdd.pStdPPSs = &m_pps;
	VkVideoEncodeH264SessionParametersCreateInfoKHR encodeInfo{
		VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
	encodeInfo.maxStdSPSCount = encodeInfo.maxStdPPSCount = 1;
	encodeInfo.pParametersAddInfo = &encodeAdd;
	VkVideoDecodeH264SessionParametersAddInfoKHR decodeAdd{
		VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_ADD_INFO_KHR};
	decodeAdd.stdSPSCount = decodeAdd.stdPPSCount = 1;
	decodeAdd.pStdSPSs = &m_sps;
	decodeAdd.pStdPPSs = &m_pps;
	VkVideoDecodeH264SessionParametersCreateInfoKHR decodeInfo{
		VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_SESSION_PARAMETERS_CREATE_INFO_KHR};
	decodeInfo.maxStdSPSCount = decodeInfo.maxStdPPSCount = 1;
	decodeInfo.pParametersAddInfo = &decodeAdd;
	VkVideoSessionParametersCreateInfoKHR parameters{VK_STRUCTURE_TYPE_VIDEO_SESSION_PARAMETERS_CREATE_INFO_KHR};
	parameters.videoSession = session;
	parameters.pNext = encode ? static_cast<void*>(&encodeInfo) : static_cast<void*>(&decodeInfo);
	CheckVulkan(m_createParameters(device, &parameters, nullptr, encode ? &m_encodeParameters : &m_decodeParameters),
				"Create H.264 parameters");
}

VkImage VulkanVideoSession::CreateImage(VkImageUsageFlags usage, bool encode, VkImageView& view)
{
	VkVideoProfileListInfoKHR profiles{VK_STRUCTURE_TYPE_VIDEO_PROFILE_LIST_INFO_KHR};
	profiles.profileCount = 1;
	profiles.pProfiles = &m_profiles[encode ? 0 : 1];
	VkPhysicalDeviceVideoFormatInfoKHR formatInfo{VK_STRUCTURE_TYPE_PHYSICAL_DEVICE_VIDEO_FORMAT_INFO_KHR, &profiles};
	formatInfo.imageUsage = usage;
	auto formats = reinterpret_cast<PFN_vkGetPhysicalDeviceVideoFormatPropertiesKHR>(
		vkGetInstanceProcAddr(m_device->binding.instance, "vkGetPhysicalDeviceVideoFormatPropertiesKHR"));
	std::uint32_t count = 0;
	CheckVulkan(formats(m_device->binding.physicalDevice, &formatInfo, &count, nullptr), "Query NV12 video formats");
	std::vector<VkVideoFormatPropertiesKHR> available(count, {VK_STRUCTURE_TYPE_VIDEO_FORMAT_PROPERTIES_KHR});
	CheckVulkan(formats(m_device->binding.physicalDevice, &formatInfo, &count, available.data()),
				"Read NV12 video formats");
	const auto selected = std::find_if(available.begin(), available.end(), [](const auto& format) {
		return format.format == VK_FORMAT_G8_B8R8_2PLANE_420_UNORM && format.imageTiling == VK_IMAGE_TILING_OPTIMAL;
	});
	if (selected == available.end())
		throw VulkanError(VK_ERROR_FORMAT_NOT_SUPPORTED, "NV12 video usage unsupported");
	const auto device = m_device->binding.device;
	VkImageCreateInfo image{VK_STRUCTURE_TYPE_IMAGE_CREATE_INFO, &profiles};
	image.flags = selected->imageCreateFlags;
	image.imageType = VK_IMAGE_TYPE_2D;
	image.format = selected->format;
	image.extent = {m_extent.width, m_extent.height, 1};
	image.mipLevels = image.arrayLayers = 1;
	image.samples = VK_SAMPLE_COUNT_1_BIT;
	image.usage = usage;
	std::array families{m_device->binding.queueFamily, encode ? m_device->encodeFamily : m_device->decodeFamily};
	if (families[0] != families[1])
	{
		image.sharingMode = VK_SHARING_MODE_CONCURRENT;
		image.queueFamilyIndexCount = 2;
		image.pQueueFamilyIndices = families.data();
	}
	m_memory.reserve(m_memory.size() + 1);
	VkImage result = VK_NULL_HANDLE;
	CheckVulkan(vkCreateImage(device, &image, nullptr, &result), "Create NV12 video image");
	try
	{
		VkMemoryRequirements requirements;
		vkGetImageMemoryRequirements(device, result, &requirements);
		VkMemoryDedicatedAllocateInfo dedicated{VK_STRUCTURE_TYPE_MEMORY_DEDICATED_ALLOCATE_INFO};
		dedicated.image = result;
		VkMemoryAllocateInfo allocation{VK_STRUCTURE_TYPE_MEMORY_ALLOCATE_INFO, &dedicated};
		allocation.allocationSize = requirements.size;
		allocation.memoryTypeIndex =
			m_device->MemoryType(requirements.memoryTypeBits, VK_MEMORY_PROPERTY_DEVICE_LOCAL_BIT);
		VkDeviceMemory memory;
		CheckVulkan(vkAllocateMemory(device, &allocation, nullptr, &memory), "Allocate NV12 image");
		m_memory.push_back(memory);
		CheckVulkan(vkBindImageMemory(device, result, memory, 0), "Bind NV12 image");
		VkImageViewCreateInfo create{VK_STRUCTURE_TYPE_IMAGE_VIEW_CREATE_INFO};
		create.image = result;
		create.viewType = VK_IMAGE_VIEW_TYPE_2D;
		create.format = image.format;
		create.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		CheckVulkan(vkCreateImageView(device, &create, nullptr, &view), "Create NV12 video view");
	}
	catch (...)
	{
		vkDestroyImage(device, result, nullptr);
		throw;
	}
	return result;
}

void VulkanVideoSession::Begin(bool encode)
{
	if (m_uncertain)
		throw VulkanError(VK_ERROR_DEVICE_LOST, "Video session completion is unknown");
	const auto command = encode ? m_encodeCommand : m_decodeCommand;
	CheckVulkan(vkResetCommandBuffer(command, 0), "Reset video command");
	VkCommandBufferBeginInfo begin{VK_STRUCTURE_TYPE_COMMAND_BUFFER_BEGIN_INFO};
	begin.flags = VK_COMMAND_BUFFER_USAGE_ONE_TIME_SUBMIT_BIT;
	CheckVulkan(vkBeginCommandBuffer(command, &begin), "Begin video command");
	vkCmdResetQueryPool(command, encode ? m_query : m_decodeQuery, 0, 1);
	VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
	barrier.image = encode ? m_dpb : m_output;
	barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
	const bool initialized = encode ? m_encodeInitialized : m_decodeInitialized;
	barrier.oldLayout = initialized ? (encode ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR : VK_IMAGE_LAYOUT_GENERAL)
									: VK_IMAGE_LAYOUT_UNDEFINED;
	barrier.newLayout = encode ? VK_IMAGE_LAYOUT_VIDEO_ENCODE_DPB_KHR : VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
	barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
	barrier.srcAccessMask = initialized ? VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT : 0;
	barrier.dstAccessMask = VK_ACCESS_MEMORY_READ_BIT | VK_ACCESS_MEMORY_WRITE_BIT;
	// The private DPB transition must be visible to the following video writes.
	vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, 0, 0, nullptr,
						 0, nullptr, 1, &barrier);
	VkVideoPictureResourceInfoKHR picture{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
	picture.codedExtent = m_extent;
	picture.imageViewBinding = encode ? m_dpbView : m_outputView;
	VkVideoReferenceSlotInfoKHR slot{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR};
	slot.slotIndex = -1;
	slot.pPictureResource = &picture;
	VkVideoBeginCodingInfoKHR coding{VK_STRUCTURE_TYPE_VIDEO_BEGIN_CODING_INFO_KHR};
	coding.videoSession = encode ? m_encode : m_decode;
	coding.videoSessionParameters = encode ? m_encodeParameters : m_decodeParameters;
	coding.referenceSlotCount = 1;
	coding.pReferenceSlots = &slot;
	VkVideoEncodeRateControlInfoKHR rate{VK_STRUCTURE_TYPE_VIDEO_ENCODE_RATE_CONTROL_INFO_KHR};
	rate.rateControlMode = VK_VIDEO_ENCODE_RATE_CONTROL_MODE_DISABLED_BIT_KHR;
	if (encode && initialized)
		coding.pNext = &rate;
	m_begin(command, &coding);
	if (!initialized)
	{
		VkVideoCodingControlInfoKHR reset{VK_STRUCTURE_TYPE_VIDEO_CODING_CONTROL_INFO_KHR};
		reset.flags = VK_VIDEO_CODING_CONTROL_RESET_BIT_KHR;
		m_control(command, &reset);
		if (encode)
		{
			reset.pNext = &rate;
			reset.flags = VK_VIDEO_CODING_CONTROL_ENCODE_RATE_CONTROL_BIT_KHR;
			m_control(command, &reset);
		}
	}
}

void VulkanVideoSession::Finish(bool encode, VkSemaphore wait, VkSemaphore signal)
{
	const auto command = encode ? m_encodeCommand : m_decodeCommand;
	VkVideoEndCodingInfoKHR end{VK_STRUCTURE_TYPE_VIDEO_END_CODING_INFO_KHR};
	m_end(command, &end);
	if (!encode)
	{
		VkImageMemoryBarrier barrier{VK_STRUCTURE_TYPE_IMAGE_MEMORY_BARRIER};
		barrier.image = m_output;
		barrier.srcQueueFamilyIndex = barrier.dstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
		barrier.oldLayout = VK_IMAGE_LAYOUT_VIDEO_DECODE_DPB_KHR;
		barrier.newLayout = VK_IMAGE_LAYOUT_GENERAL;
		barrier.srcAccessMask = VK_ACCESS_MEMORY_WRITE_BIT;
		barrier.subresourceRange = {VK_IMAGE_ASPECT_COLOR_BIT, 0, 1, 0, 1};
		vkCmdPipelineBarrier(command, VK_PIPELINE_STAGE_ALL_COMMANDS_BIT, VK_PIPELINE_STAGE_BOTTOM_OF_PIPE_BIT, 0, 0,
							 nullptr, 0, nullptr, 1, &barrier);
	}
	CheckVulkan(vkEndCommandBuffer(command), "End video command");
	const auto device = m_device->binding.device;
	CheckVulkan(vkResetFences(device, 1, &m_fence), "Reset video fence");
	VkQueue queue;
	vkGetDeviceQueue(device, encode ? m_device->encodeFamily : m_device->decodeFamily, 0, &queue);
	VkSubmitInfo submit{VK_STRUCTURE_TYPE_SUBMIT_INFO};
	submit.commandBufferCount = 1;
	submit.pCommandBuffers = &command;
	const VkPipelineStageFlags stage = VK_PIPELINE_STAGE_ALL_COMMANDS_BIT;
	submit.waitSemaphoreCount = wait ? 1 : 0;
	submit.pWaitSemaphores = &wait;
	submit.pWaitDstStageMask = &stage;
	submit.signalSemaphoreCount = signal ? 1 : 0;
	submit.pSignalSemaphores = &signal;
	{
		std::lock_guard lock(m_device->queueMutex);
		m_uncertain = true;
		CheckVulkan(vkQueueSubmit(queue, 1, &submit, m_fence), "Submit video work");
	}
	CheckVulkan(vkWaitForFences(device, 1, &m_fence, VK_TRUE, 5000000000ULL), "Wait for video completion");
	m_uncertain = false;
	(encode ? m_encodeInitialized : m_decodeInitialized) = true;
}

VulkanH264Picture VulkanVideoSession::Encode(VkSemaphore inputReady)
{
	Begin(true);
	StdVideoEncodeH264SliceHeader slice{};
	slice.slice_type = STD_VIDEO_H264_SLICE_TYPE_I;
	slice.disable_deblocking_filter_idc = STD_VIDEO_H264_DISABLE_DEBLOCKING_FILTER_IDC_DISABLED;
	VkVideoEncodeH264NaluSliceInfoKHR nalu{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_NALU_SLICE_INFO_KHR};
	nalu.constantQp = std::clamp(22, m_h264Caps.minQp, m_h264Caps.maxQp);
	nalu.pStdSliceHeader = &slice;
	StdVideoEncodeH264PictureInfo info{};
	info.flags.IdrPicFlag = info.flags.is_reference = 1;
	info.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_IDR;
	VkVideoEncodeH264PictureInfoKHR h264{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PICTURE_INFO_KHR};
	h264.naluSliceEntryCount = 1;
	h264.pNaluSliceEntries = &nalu;
	h264.pStdPictureInfo = &info;
	StdVideoEncodeH264ReferenceInfo reference{};
	reference.primary_pic_type = STD_VIDEO_H264_PICTURE_TYPE_IDR;
	VkVideoEncodeH264DpbSlotInfoKHR h264Slot{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_DPB_SLOT_INFO_KHR};
	h264Slot.pStdReferenceInfo = &reference;
	VkVideoPictureResourceInfoKHR reconstructed{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
	reconstructed.codedExtent = m_extent;
	reconstructed.imageViewBinding = m_dpbView;
	VkVideoReferenceSlotInfoKHR slot{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, &h264Slot};
	slot.slotIndex = 0;
	slot.pPictureResource = &reconstructed;
	VkVideoEncodeInfoKHR encode{VK_STRUCTURE_TYPE_VIDEO_ENCODE_INFO_KHR, &h264};
	encode.dstBuffer = m_bitstream;
	encode.dstBufferRange = m_bitstreamSize;
	encode.srcPictureResource = {VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
	encode.srcPictureResource.codedExtent = m_extent;
	encode.srcPictureResource.imageViewBinding = m_inputView;
	encode.pSetupReferenceSlot = &slot;
	vkCmdBeginQuery(m_encodeCommand, m_query, 0, 0);
	m_encodeVideo(m_encodeCommand, &encode);
	vkCmdEndQuery(m_encodeCommand, m_query, 0);
	Finish(true, inputReady, VK_NULL_HANDLE);
	std::array<std::uint32_t, 3> feedback{};
	CheckVulkan(vkGetQueryPoolResults(m_device->binding.device, m_query, 0, 1, sizeof(feedback), feedback.data(),
									  sizeof(feedback), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR),
				"Read H.264 encoding result");
	if (feedback[2] != VK_QUERY_RESULT_STATUS_COMPLETE_KHR || !feedback[1] || feedback[0] > m_bitstreamSize ||
		feedback[1] > m_bitstreamSize - feedback[0])
		throw std::runtime_error("H.264 encoder did not produce a complete picture");
	VulkanH264Picture picture;
	picture.bytes = m_headers;
	picture.sliceOffset = static_cast<std::uint32_t>(picture.bytes.size());
	const auto start = static_cast<const std::uint8_t*>(m_mapped) + feedback[0];
	picture.bytes.insert(picture.bytes.end(), start, start + feedback[1]);
	return picture;
}

void VulkanVideoSession::Decode(const VulkanH264Picture& picture, VkSemaphore outputReady)
{
	if (m_uncertain)
		throw std::runtime_error("Video completion unknown; bitstream cannot be reused");
	if (picture.bytes.empty() || picture.bytes.size() > m_bitstreamSize || picture.sliceOffset >= picture.bytes.size())
		throw std::invalid_argument("Invalid H.264 history picture");
	std::memcpy(m_mapped, picture.bytes.data(), picture.bytes.size());
	Begin(false);
	StdVideoDecodeH264PictureInfo info{};
	info.flags.is_intra = info.flags.IdrPicFlag = info.flags.is_reference = 1;
	VkVideoDecodeH264PictureInfoKHR h264{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PICTURE_INFO_KHR};
	h264.pStdPictureInfo = &info;
	h264.sliceCount = 1;
	h264.pSliceOffsets = &picture.sliceOffset;
	StdVideoDecodeH264ReferenceInfo reference{};
	VkVideoDecodeH264DpbSlotInfoKHR h264Slot{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_DPB_SLOT_INFO_KHR};
	h264Slot.pStdReferenceInfo = &reference;
	VkVideoPictureResourceInfoKHR output{VK_STRUCTURE_TYPE_VIDEO_PICTURE_RESOURCE_INFO_KHR};
	output.codedExtent = m_extent;
	output.imageViewBinding = m_outputView;
	VkVideoReferenceSlotInfoKHR slot{VK_STRUCTURE_TYPE_VIDEO_REFERENCE_SLOT_INFO_KHR, &h264Slot};
	slot.slotIndex = 0;
	slot.pPictureResource = &output;
	VkVideoDecodeInfoKHR decode{VK_STRUCTURE_TYPE_VIDEO_DECODE_INFO_KHR, &h264};
	decode.srcBuffer = m_bitstream;
	const auto alignment = m_decodeCaps.minBitstreamBufferSizeAlignment;
	decode.srcBufferRange = (picture.bytes.size() + alignment - 1) / alignment * alignment;
	decode.dstPictureResource = output;
	decode.pSetupReferenceSlot = &slot;
	vkCmdBeginQuery(m_decodeCommand, m_decodeQuery, 0, 0);
	m_decodeVideo(m_decodeCommand, &decode);
	vkCmdEndQuery(m_decodeCommand, m_decodeQuery, 0);
	Finish(false, VK_NULL_HANDLE, outputReady);
	VkQueryResultStatusKHR status = VK_QUERY_RESULT_STATUS_NOT_READY_KHR;
	CheckVulkan(vkGetQueryPoolResults(m_device->binding.device, m_decodeQuery, 0, 1, sizeof(status), &status,
									  sizeof(status), VK_QUERY_RESULT_WITH_STATUS_BIT_KHR),
				"Read H.264 decode status");
	if (status != VK_QUERY_RESULT_STATUS_COMPLETE_KHR)
		throw std::runtime_error("H.264 decoder did not produce a complete picture");
}
} // namespace simxr
