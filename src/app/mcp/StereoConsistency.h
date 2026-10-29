#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
struct StereoFrusta
{
	float infinity = 0, focal = 0, ipd = 0;
};

struct StereoConsistencyReport
{
	nlohmann::json document;
	std::vector<std::uint8_t> sheet;
	std::uint32_t sheetWidth = 0, sheetHeight = 0;
};

// Correspondence assumes equal horizontal scale and identical vertical projection.
bool StereoProjectionsMatch(const nlohmann::json& views);

// The two RGB8 inputs already have the same bounded analysis dimensions.
StereoConsistencyReport AnalyzeStereoConsistency(std::span<const std::uint8_t> left,
												 std::span<const std::uint8_t> right, std::uint32_t width,
												 std::uint32_t height, std::uint32_t sourceHeight, float threshold,
												 std::optional<StereoFrusta> frusta, bool includeImage,
												 const std::function<void()>& check);
} // namespace simxr::app::mcp
