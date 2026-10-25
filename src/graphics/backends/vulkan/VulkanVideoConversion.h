#pragma once
#include "graphics/backends/vulkan/VulkanInspection.h"
#include "graphics/backends/vulkan/VulkanVideoSession.h"

namespace simxr
{
class VulkanVideoConversion
{
  public:
	VulkanVideoConversion(std::shared_ptr<VulkanDevice> device, VkExtent2D extent);
	~VulkanVideoConversion();
	VulkanH264Picture Encode(std::shared_ptr<const VulkanInspectionFrame> source);
	std::shared_ptr<VulkanImage> Decode(const VulkanH264Picture& picture, VkExtent2D extent, std::uint32_t layers,
										std::shared_ptr<FrameReservation> reservation = {});
	VkExtent2D Extent() const noexcept
	{
		return m_session->Extent();
	}

  private:
	struct FailedOwner
	{
		VulkanNativePreservation<FailedOwner, 4>::Reservation preservation{
			"Four failed video conversions retain native resources"};
		std::shared_ptr<VulkanDevice> device;
		std::shared_ptr<VulkanVideoSession> session;
		std::shared_ptr<const VulkanInspectionFrame> source;
		std::shared_ptr<VulkanImage> output;
		std::shared_ptr<FrameReservation> reservation;
	};
	void Initialize();
	void Destroy() noexcept;
	void Begin();
	void Submit(VkSemaphore wait, VkSemaphore signal);
	void Bind(bool encode, VkExtent2D extent, std::uint32_t layers, bool srgb);
	void UpdateView(VkImage image, VkFormat format, std::uint32_t layers, bool encode);
	std::shared_ptr<VulkanDevice> m_device;
	std::shared_ptr<VulkanVideoSession> m_session;
	std::unique_ptr<FailedOwner> m_failedOwner;
	VkBuffer m_buffer = VK_NULL_HANDLE;
	VkDeviceMemory m_memory = VK_NULL_HANDLE;
	VkCommandPool m_pool = VK_NULL_HANDLE;
	VkCommandBuffer m_command = VK_NULL_HANDLE;
	VkFence m_fence = VK_NULL_HANDLE;
	VkSemaphore m_ready = VK_NULL_HANDLE, m_decoded = VK_NULL_HANDLE;
	VkDescriptorPool m_descriptors = VK_NULL_HANDLE;
	VkDescriptorSetLayout m_setLayout = VK_NULL_HANDLE;
	VkDescriptorSet m_set = VK_NULL_HANDLE;
	VkPipelineLayout m_layout = VK_NULL_HANDLE;
	VkPipeline m_encode = VK_NULL_HANDLE, m_decode = VK_NULL_HANDLE;
	VkSampler m_sampler = VK_NULL_HANDLE;
	VkImageView m_view = VK_NULL_HANDLE;
	bool m_uncertain = false, m_initialized = false;
};
} // namespace simxr
