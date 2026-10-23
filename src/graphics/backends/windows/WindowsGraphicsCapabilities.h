#pragma once

#include <openxr/openxr.h>
#include <cstdint>
#include <span>

namespace simxr::windows
{
std::span<const std::int64_t> Direct3DSwapchainFormats();
XrResult Direct3DMaxSampleCount(bool d3d11, bool d3d12, std::uint32_t& output);
}
