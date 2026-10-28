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
nlohmann::json VideoSaveDefinition();
std::optional<nlohmann::json> SaveVideo(McpApplication& application, McpDebug& debug, const nlohmann::json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation);
}
}
