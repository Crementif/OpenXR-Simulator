#pragma once
#include "graphics/ArchivedFrame.h"
#include "graphics/backends/vulkan/VulkanVideoConversion.h"
#include <mutex>

namespace simxr
{
class VulkanVideoHistory : public std::enable_shared_from_this<VulkanVideoHistory>
{
  public:
	VulkanVideoHistory(std::shared_ptr<VulkanDevice> device, bool timeline)
		: m_device(std::move(device)), m_timeline(timeline)
	{
	}
	std::shared_ptr<const ArchivedFrame> Encode(std::shared_ptr<const VulkanInspectionFrame> source);
	std::shared_ptr<const FrameOutput> Decode(const VulkanH264Picture& picture, VkExtent2D extent, std::uint32_t layers,
											  std::span<const FrameView> views, const FramePool& budget);

  private:
	void Configure(VkExtent2D extent, std::uint32_t layers);
	std::shared_ptr<VulkanDevice> m_device;
	bool m_timeline;
	std::mutex m_mutex;
	std::unique_ptr<VulkanVideoConversion> m_conversion;
};
} // namespace simxr
