#pragma once
#include <cstdint>
#include <span>

namespace simxr
{
struct H264Frame
{
	std::span<const std::uint8_t> bytes;
	std::uint32_t width = 0, height = 0;
};
} // namespace simxr
