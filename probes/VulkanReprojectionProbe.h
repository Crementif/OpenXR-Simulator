#pragma once

#include <memory>
#include <vector>

namespace simxr
{
class VulkanInspectionFrame;
namespace probe
{
void VerifyRetainedComposition(std::vector<std::shared_ptr<VulkanInspectionFrame>> linear,
	std::vector<std::shared_ptr<VulkanInspectionFrame>> srgb, bool verifyPixels);
}
} // namespace simxr
