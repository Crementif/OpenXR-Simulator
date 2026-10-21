#pragma once

#include <cstdint>
#include <memory>
#include <string>

namespace simxr
{
class VulkanInspectionFrame;
namespace probe
{
void VerifyImageDelivery(std::shared_ptr<VulkanInspectionFrame> original,
						 std::shared_ptr<VulkanInspectionFrame> resized, bool verifyPixels);
void VerifyControlImage(const std::string& socket, std::uint64_t session, std::uint64_t generation,
						std::uint64_t capture, const std::string& gate, bool verifyPixels);
} // namespace probe
} // namespace simxr
