#pragma once

#include <cstdint>
#include <functional>
#include <span>

#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
// A single central-window disparity heuristic, not a stereo-consistency test.
nlohmann::json AnalyzeStereo(std::span<const std::uint8_t> left, std::span<const std::uint8_t> right,
	std::uint32_t width, std::uint32_t height, std::uint32_t sourceWidth, const std::function<void()>& check);
}
