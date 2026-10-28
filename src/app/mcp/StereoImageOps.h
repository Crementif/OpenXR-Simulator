#pragma once

#include <vector>

namespace simxr::app::mcp
{
std::vector<float> StereoBoxMean(const std::vector<float>& input, int width, int height, int radiusY, int radiusX);
} // namespace simxr::app::mcp
