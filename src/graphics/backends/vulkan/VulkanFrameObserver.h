#pragma once

#include "graphics/backends/vulkan/VulkanReadbackBatch.h"
#include "presentation/FrameTypes.h"
#include <optional>

namespace simxr
{
struct LegacyCaptureRequest;
struct CompositionPlan;
struct LayerImageDescription;
struct VulkanCaptureSource
{
	VulkanReadbackRegion region;
	XrSwapchain swapchain = XR_NULL_HANDLE;
	std::uint32_t imageIndex = 0;
};

// Synchronous binding access; observers retain resolved resources, never this view.
class VulkanCaptureBindings
{
  public:
	virtual ~VulkanCaptureBindings() = default;
	virtual std::optional<VulkanCaptureSource> ResolveSource(const LayerImageDescription& image) const = 0;
	virtual void MarkSourceUsed(const VulkanCaptureSource& source,
								const std::shared_ptr<VulkanReadbackBatch>& completion) const noexcept = 0;
};

class VulkanFrameObserver
{
  public:
	virtual ~VulkanFrameObserver() = default;
	virtual void Request(LegacyCaptureRequest request, FrameId frame) = 0;
	virtual void Observe(const XrFrameEndInfo& frame, const CompositionPlan& composition,
						 const VulkanCaptureBindings& bindings) noexcept = 0;
	virtual void Poll() = 0;
	virtual VkResult Close(std::uint64_t timeout) = 0;
};
} // namespace simxr
