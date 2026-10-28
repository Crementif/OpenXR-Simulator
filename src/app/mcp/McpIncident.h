#pragma once

#include <memory>
#include <cstdint>
#include <optional>
#include <string>
#include <nlohmann/json.hpp>

namespace simxr::app
{
class McpApplication;
class McpCancellation;
namespace mcp
{
bool IncidentHandles(const std::string& name);
std::uint64_t IncidentIdentifier(const nlohmann::json& value, const char* field);
nlohmann::json IncidentDefinitions();
std::optional<nlohmann::json> CaptureIncident(McpApplication& application, const std::string& name,
	const nlohmann::json& arguments, const std::shared_ptr<McpCancellation>& cancellation, bool allowMissing = false);
}
}
