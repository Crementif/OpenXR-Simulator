#pragma once

#include <memory>
#include <optional>
#include <nlohmann/json.hpp>

namespace simxr::app
{
class McpApplication;
class McpCancellation;
class McpDebug;
namespace mcp
{
nlohmann::json ImageSaveDefinition();
std::optional<nlohmann::json> SaveImage(McpApplication& app, McpDebug& debug, const nlohmann::json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation);
}
}
