#pragma once

#include <cstdint>
#include <openxr/openxr.h>
#include <span>

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace simxr::windows
{
std::span<const std::int64_t> Direct3DSwapchainFormats();
XrResult Direct3DAdapterLuid(LUID& output);
XrResult Direct3DMaxSampleCount(bool d3d11, bool d3d12, std::uint32_t& output);
} // namespace simxr::windows
