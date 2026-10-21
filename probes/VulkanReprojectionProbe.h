#pragma once

#include <memory>
#include <vector>

namespace simxr
{
class VulkanInspectionFrame;
struct VulkanImagePixels;
struct CompositionPlan;
namespace probe
{
void VerifyCompositionPixels(const CompositionPlan& plan, const VulkanInspectionFrame& output,
							 const std::vector<std::shared_ptr<const VulkanImagePixels>>& sources);
void VerifyRetainedComposition(std::vector<std::shared_ptr<VulkanInspectionFrame>> linear,
	std::vector<std::shared_ptr<VulkanInspectionFrame>> srgb, bool verifyPixels);
}
} // namespace simxr
