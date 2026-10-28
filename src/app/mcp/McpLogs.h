#pragma once

#include <memory>
#include <optional>
#include <nlohmann/json.hpp>

namespace simxr::app
{
class McpApplication;
class McpCancellation;
namespace mcp
{
nlohmann::json LogDefinition();
nlohmann::json ClearLogDefinition();
std::optional<nlohmann::json> LogCall(McpApplication& application, const nlohmann::json& arguments, bool clear,
	const std::shared_ptr<McpCancellation>& cancellation);
}
}
