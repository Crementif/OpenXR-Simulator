#pragma once

#include "graphics/backends/vulkan/VulkanNativePreservation.h"
#include "graphics/backends/vulkan/VulkanResources.h"
#include <vector>

namespace simxr
{
// A single independent H.264 picture. Parameter sets precede the IDR slice.
struct VulkanH264Picture
{
	std::vector<std::uint8_t> bytes;
	std::uint32_t sliceOffset = 0;
};

// Serial use by the history worker. All images and bitstream buffers are private
// to this session; the caller supplies already completed NV12 conversion output.
class VulkanVideoSession
{
  public:
	VulkanVideoSession(std::shared_ptr<VulkanDevice> device, VkExtent2D extent);
	~VulkanVideoSession();
	VulkanVideoSession(const VulkanVideoSession&) = delete;
	VulkanVideoSession& operator=(const VulkanVideoSession&) = delete;
	VkImage Input() const noexcept
	{
		return m_input;
	}
	VkImage Output() const noexcept
	{
		return m_output;
	}
	VkExtent2D Extent() const noexcept
	{
		return m_extent;
	}
	VulkanH264Picture Encode(VkSemaphore inputReady = VK_NULL_HANDLE);
	void Decode(const VulkanH264Picture& picture, VkSemaphore outputReady = VK_NULL_HANDLE);

  private:
	void Initialize();
	void Destroy() noexcept;
	void CreateSession(bool encode);
	VkImage CreateImage(VkImageUsageFlags usage, bool encode, VkImageView& view);
	void Begin(bool encode);
	void Finish(bool encode, VkSemaphore wait, VkSemaphore signal);
	struct FailedOwner
	{
		VulkanNativePreservation<FailedOwner, 4>::Reservation preservation{
			"Four failed video sessions retain native resources"};
		std::shared_ptr<VulkanDevice> device;
	};
	std::unique_ptr<FailedOwner> m_failedOwner;
	bool m_uncertain = false;
	std::shared_ptr<VulkanDevice> m_device;
	VkExtent2D m_extent;
	VkVideoSessionKHR m_encode = VK_NULL_HANDLE, m_decode = VK_NULL_HANDLE;
	VkVideoSessionParametersKHR m_encodeParameters = VK_NULL_HANDLE, m_decodeParameters = VK_NULL_HANDLE;
	VkImage m_input = VK_NULL_HANDLE, m_dpb = VK_NULL_HANDLE, m_output = VK_NULL_HANDLE;
	VkImageView m_inputView = VK_NULL_HANDLE, m_dpbView = VK_NULL_HANDLE, m_outputView = VK_NULL_HANDLE;
	std::vector<VkDeviceMemory> m_memory;
	VkBuffer m_bitstream = VK_NULL_HANDLE;
	VkDeviceMemory m_bitstreamMemory = VK_NULL_HANDLE;
	void* m_mapped = nullptr;
	VkDeviceSize m_bitstreamSize = 0;
	VkQueryPool m_query = VK_NULL_HANDLE, m_decodeQuery = VK_NULL_HANDLE;
	VkCommandPool m_encodePool = VK_NULL_HANDLE, m_decodePool = VK_NULL_HANDLE;
	VkCommandBuffer m_encodeCommand = VK_NULL_HANDLE, m_decodeCommand = VK_NULL_HANDLE;
	VkFence m_fence = VK_NULL_HANDLE;
	bool m_encodeInitialized = false, m_decodeInitialized = false;
	std::vector<std::uint8_t> m_headers;
	StdVideoH264SequenceParameterSetVui m_vui{};
	StdVideoH264SequenceParameterSet m_sps{};
	StdVideoH264PictureParameterSet m_pps{};
	VkVideoCapabilitiesKHR m_encodeCaps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
	VkVideoCapabilitiesKHR m_decodeCaps{VK_STRUCTURE_TYPE_VIDEO_CAPABILITIES_KHR};
	VkVideoEncodeH264CapabilitiesKHR m_h264Caps{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_CAPABILITIES_KHR};
	VkVideoEncodeH264ProfileInfoKHR m_encodeProfile{VK_STRUCTURE_TYPE_VIDEO_ENCODE_H264_PROFILE_INFO_KHR};
	VkVideoDecodeH264ProfileInfoKHR m_decodeProfile{VK_STRUCTURE_TYPE_VIDEO_DECODE_H264_PROFILE_INFO_KHR};
	VkVideoProfileInfoKHR m_profiles[2]{};
	PFN_vkCreateVideoSessionKHR m_createSession = nullptr;
	PFN_vkDestroyVideoSessionKHR m_destroySession = nullptr;
	PFN_vkGetVideoSessionMemoryRequirementsKHR m_getMemory = nullptr;
	PFN_vkBindVideoSessionMemoryKHR m_bindMemory = nullptr;
	PFN_vkCreateVideoSessionParametersKHR m_createParameters = nullptr;
	PFN_vkDestroyVideoSessionParametersKHR m_destroyParameters = nullptr;
	PFN_vkGetEncodedVideoSessionParametersKHR m_getParameters = nullptr;
	PFN_vkCmdBeginVideoCodingKHR m_begin = nullptr;
	PFN_vkCmdEndVideoCodingKHR m_end = nullptr;
	PFN_vkCmdControlVideoCodingKHR m_control = nullptr;
	PFN_vkCmdEncodeVideoKHR m_encodeVideo = nullptr;
	PFN_vkCmdDecodeVideoKHR m_decodeVideo = nullptr;
};
} // namespace simxr
