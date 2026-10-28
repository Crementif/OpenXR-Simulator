#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

namespace simxr::app::mcp
{
struct StereoSurface
{
	std::uint32_t width = 0, height = 0;
	int minimumDisparity = 0, maximumDisparity = 0;
	std::vector<float> disparity, uniqueness;
	std::vector<std::uint8_t> trusted;
};

// Inputs are tightly packed RGB8 images already reduced to the analysis size.
StereoSurface MatchStereoSurface(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right,
								 std::uint32_t width, std::uint32_t height, std::optional<float> infinity,
								 const std::function<void()>& check);
} // namespace simxr::app::mcp
