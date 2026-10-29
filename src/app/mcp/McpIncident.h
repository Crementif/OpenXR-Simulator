#pragma once

#include "app/services/ImageExport.h"

#include <cstdint>
#include <filesystem>
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
struct IncidentRequest
{
	std::string application;
	std::filesystem::path directory;
	double timeout = 5;
	unsigned maximum = 12;
	bool ui = false;
	bool analyze = false;
	bool includeImage = true;
	bool allowMissing = false;
};
struct IncidentCapture
{
	nlohmann::json metadata;
	std::optional<EncodedImage> image;
};
std::optional<IncidentCapture> CaptureIncident(McpApplication& application, const IncidentRequest& request,
	const std::shared_ptr<McpCancellation>& cancellation);
bool IncidentHandles(const std::string& name);
std::uint64_t IncidentIdentifier(const nlohmann::json& value, const char* field);
nlohmann::json IncidentDefinitions();
std::optional<nlohmann::json> CaptureIncident(McpApplication& application, const std::string& name,
	const nlohmann::json& arguments, const std::shared_ptr<McpCancellation>& cancellation);
}
}
