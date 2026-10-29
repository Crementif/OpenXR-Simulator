#pragma once

#include <cstdint>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
std::vector<std::uint8_t> StereoConsistencyImage(
	std::span<const std::uint8_t> left, std::span<const std::uint8_t> right, const std::vector<float>& luminance,
	const std::vector<float>& residual, const std::vector<std::uint8_t>& valid, const std::vector<float>& surface,
	const nlohmann::json& regions, unsigned width, unsigned height, float threshold, int maximumDisparity);
} // namespace simxr::app::mcp
