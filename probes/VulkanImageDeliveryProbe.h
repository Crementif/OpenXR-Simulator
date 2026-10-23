#pragma once

#include "graphics/SharedImageTypes.h"

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>

namespace simxr
{
class VulkanInspectionFrame;
namespace probe
{
void VerifyConsumerImage(const std::shared_ptr<VulkanInspectionFrame>& source, std::span<const std::byte> expected);
void VerifyImageDelivery(std::shared_ptr<VulkanInspectionFrame> original,
						 std::shared_ptr<VulkanInspectionFrame> resized, bool verifyPixels,
						 const std::function<bool(SharedImageKey)>& retired);
void VerifyControlImage(const std::string& socket, std::uint64_t session, std::uint64_t generation,
						std::uint64_t capture, const std::filesystem::path& gate, bool verifyPixels);
} // namespace probe
} // namespace simxr
