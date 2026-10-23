#pragma once

#include "graphics/SharedImageTypes.h"

#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <string>

namespace simxr
{
class VulkanInspectionFrame;
namespace probe
{
void VerifyImageDelivery(std::shared_ptr<VulkanInspectionFrame> original,
						 std::shared_ptr<VulkanInspectionFrame> resized, bool verifyPixels,
						 const std::function<bool(SharedImageKey)>& retired);
void VerifyControlImage(const std::string& socket, std::uint64_t session, std::uint64_t generation,
						std::uint64_t capture, const std::filesystem::path& gate, bool verifyPixels);
} // namespace probe
} // namespace simxr
