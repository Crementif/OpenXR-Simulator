#include "graphics/backends/vulkan/VulkanVideoHistory.h"
#include "Pch.h"

namespace simxr
{
class VulkanArchivedFrame final : public ArchivedFrame
{
  public:
	std::shared_ptr<VulkanVideoHistory> codec;
	VulkanH264Picture picture;
	VkExtent2D extent{};
	std::uint32_t layers = 0;
	std::vector<FrameView> views;
	std::uint64_t Bytes() const noexcept override
	{
		return sizeof(*this) + picture.bytes.capacity() + views.capacity() * sizeof(FrameView);
	}
	std::shared_ptr<const FrameOutput> Decode(const FramePool& budget) const override
	{
		return codec->Decode(picture, extent, layers, views, budget);
	}
};
void VulkanVideoHistory::Configure(VkExtent2D extent, std::uint32_t layers)
{
	// Padding also admits small probe/previews on encoders with larger minima.
	const VkExtent2D coded{std::max(128u, (extent.width + 15) / 16 * 16) * layers,
						   std::max(128u, (extent.height + 15) / 16 * 16)};
	if (!m_conversion || m_conversion->Extent().width != coded.width || m_conversion->Extent().height != coded.height)
	{
		m_conversion.reset();
		m_conversion = std::make_unique<VulkanVideoConversion>(m_device, coded);
	}
}
std::shared_ptr<const ArchivedFrame> VulkanVideoHistory::Encode(std::shared_ptr<const VulkanInspectionFrame> source)
{
	std::lock_guard lock(m_mutex);
	auto result = std::make_shared<VulkanArchivedFrame>();
	result->codec = shared_from_this();
	result->extent = source->Image().extent;
	result->layers = source->Image().layers;
	result->views.assign(source->Views().begin(), source->Views().end());
	Configure(result->extent, result->layers);
	result->picture = m_conversion->Encode(std::move(source));
	return result;
}
std::shared_ptr<const FrameOutput> VulkanVideoHistory::Decode(const VulkanH264Picture& picture, VkExtent2D extent,
															  std::uint32_t layers, std::span<const FrameView> views,
															  const FramePool& budget)
{
	auto reservation = budget.Reserve();
	if (!reservation)
		return {};
	std::lock_guard lock(m_mutex);
	Configure(extent, layers);
	auto image = m_conversion->Decode(picture, extent, layers);
	auto frame = VulkanInspectionFrame::FromImage(std::move(image), views, m_timeline);
	if (!frame->PromoteCaptureBudget(budget, std::move(reservation)))
		throw std::runtime_error("Decoded history image budget transfer failed");
	return frame;
}
} // namespace simxr
