#pragma once

#include <filesystem>
#include <optional>
#include <string>

namespace simxr::app
{
enum class McpConfigurationClient
{
	Generic,
	Codex
};

std::string McpConfiguration(const std::filesystem::path& connector,
	const std::optional<std::filesystem::path>& app, bool windowless,
	McpConfigurationClient client = McpConfigurationClient::Generic);
}
