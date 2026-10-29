#pragma once

#include <cstddef>
#include <filesystem>
#include <functional>
#include <span>
#include <vector>

#include <nlohmann/json.hpp>

namespace simxr::app::mcp
{
std::filesystem::path IncidentDirectory(const nlohmann::json& arguments);
void SaveIncident(const std::filesystem::path& directory, nlohmann::json& metadata, std::span<const std::byte> jpeg,
				  const std::vector<std::vector<std::byte>>& frames, unsigned width, unsigned height,
				  const std::function<void()>& check);
}
