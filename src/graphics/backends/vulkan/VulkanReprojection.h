#pragma once

#include "graphics/Compositor.h"
#include "graphics/RotationalSource.h"
#include "graphics/backends/vulkan/VulkanBackend.h"
#include "graphics/backends/vulkan/VulkanInspection.h"

namespace simxr
{
// Preparation records copies on the caller's serialized application queue.
// Commit follows successful submission of every Frames() entry in that batch.
// The resulting source owns only private images on the inspection device.
class VulkanReprojectionPreparation final
{
  public:
	static XrResult Prepare(VulkanBackend& backend, VulkanInspection& inspection, const FrameLayers& layers,
							const CompositionTarget& target, const PoseSample& head, const VulkanImageResolver& resolve,
							std::size_t capacity, std::unique_ptr<VulkanReprojectionPreparation>& output,
							std::function<void(SharedImageKey)> deliveryRetired = {});
	std::span<const std::shared_ptr<VulkanFrame>> Frames() const noexcept;
	std::span<const XrSwapchain> Swapchains() const noexcept;
	std::shared_ptr<const RotationalSource> Source() const noexcept;
	void Commit() noexcept;

  private:
	VulkanReprojectionPreparation() = default;
	std::vector<std::shared_ptr<VulkanFrame>> m_frames;
	std::vector<XrSwapchain> m_swapchains;
	std::vector<std::unique_ptr<VulkanInspectionTransfer>> m_transfers;
	std::shared_ptr<const RotationalSource> m_source;
};
} // namespace simxr
