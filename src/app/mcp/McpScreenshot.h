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
nlohmann::json ScreenshotDefinition();
nlohmann::json StereoValidationDefinition();
nlohmann::json StereoConsistencyDefinition();
std::optional<nlohmann::json> CaptureScreenshot(McpApplication& application, const nlohmann::json& arguments,
												 const std::shared_ptr<McpCancellation>& cancellation);
std::optional<nlohmann::json> ValidateStereo(McpApplication& application, const nlohmann::json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation);
std::optional<nlohmann::json> CaptureStereoConsistency(McpApplication& application, const nlohmann::json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation);
} // namespace mcp
} // namespace simxr::app
