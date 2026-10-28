#pragma once

#include <cstdint>

namespace simxr
{
struct ImageRegion
{
	std::uint32_t x = 0, y = 0, width = 0, height = 0;
	std::uint32_t outputWidth = 0, outputHeight = 0;
};
} // namespace simxr
