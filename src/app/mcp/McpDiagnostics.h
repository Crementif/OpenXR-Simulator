#pragma once

#include <memory>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace simxr::app
{
class McpApplication;
class McpCancellation;

namespace mcp
{
bool DiagnosticsHandles(const std::string& name);
nlohmann::json DiagnosticsDefinitions();
std::optional<nlohmann::json> RunDiagnostics(McpApplication& application, const std::string& name,
											 const nlohmann::json& arguments,
											 const std::shared_ptr<McpCancellation>& cancellation);
}
}
