#pragma once

#include "graphics/Compositor.h"
#include "graphics/RotationalSource.h"
#include <vulkan/vulkan.h>

namespace simxr
{
class VulkanBackend;
class VulkanImage;
class VulkanInspection;
namespace probe
{
struct RetainedSubmission
{
	std::shared_ptr<const RotationalSource> source;
	std::shared_ptr<const FrameOutput> baseline;
	std::shared_ptr<const FrameLayers> layers;
	CompositionTarget target;
	PoseSample head;
	VkFormat format;
};
RetainedSubmission RetainSubmittedLayers(VulkanBackend& backend, VulkanInspection& inspection,
										 const std::shared_ptr<VulkanImage>& image);
void VerifySubmittedLayers(RetainedSubmission retained, bool verifyPixels);
} // namespace probe
} // namespace simxr
