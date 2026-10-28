#pragma once

#include <memory>
#include <functional>
#include <optional>
#include <nlohmann/json.hpp>

namespace simxr::app
{
class McpApplication;
class McpCancellation;
class McpDebug;
namespace mcp
{
nlohmann::json SequenceCaptureDefinition();
void ValidateSequenceCapture(const nlohmann::json& arguments);
std::optional<nlohmann::json> CaptureSequence(McpApplication& app, McpDebug& debug, const nlohmann::json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation,
	const std::function<void(const nlohmann::json&)>& progress = {});
}
}
