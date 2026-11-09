#include "app/mcp/McpPch.h"
#include "app/mcp/McpTools.h"
#include "app/mcp/McpDebug.h"
#include "app/mcp/McpSequenceJobs.h"

namespace simxr::app
{
using Json = nlohmann::json;

Json McpToolDefinitions(bool windowless)
{
	const Json text = {{"type", "string"}};
	const Json identifier = {{"type", "string"}, {"pattern", "^[0-9a-f]{64}:[1-9][0-9]{0,19}$"}};
	Json tools = Json::array(
		{{{"name", "simulator_list_applications"},
		  {"description", "List applications in OpenXR Simulator, including their launch and connection state."},
		  {"inputSchema", {{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}}},
		 {{"name", "simulator_application_status"},
		  {"description", "Read one application's launch, runtime connection and session state."},
		  {"inputSchema",
		   {{"type", "object"},
			{"properties", {{"application_id", identifier}}},
			{"required", Json::array({"application_id"})},
			{"additionalProperties", false}}}},
		 {{"name", "simulator_stop_application"},
		  {"description", "Request a graceful close of an application launched by the simulator. "
						  "Use simulator_application_status to observe whether it exits; attached external "
						  "applications are not supported."},
		  {"inputSchema",
		   {{"type", "object"},
			{"properties", {{"application_id", identifier}}},
			{"required", Json::array({"application_id"})},
			{"additionalProperties", false}}}},
		 {{"name", "simulator_restart_application"},
		  {"description",
		   "Request a graceful restart using the app's original launch profile. "
		   "Pass the current process_id from simulator_application_status; inspect status for the outcome."},
		  {"inputSchema",
		   {{"type", "object"},
			{"properties",
			 {{"application_id", identifier}, {"process_id", {{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}}}}},
			{"required", Json::array({"application_id", "process_id"})},
			{"additionalProperties", false}}}},
		 {{"name", "simulator_launch_application"},
		  {"description", "Launch an executable using OpenXR Simulator and return its application ID. "
						  "Use simulator_application_status to observe launch and connection progress."},
		  {"inputSchema",
		   {{"type", "object"},
			{"properties",
			 {{"executable", text},
			  {"name", text},
			  {"working_directory", text},
			  {"runtime_manifest", text},
			  {"arguments", {{"type", "array"}, {"items", text}, {"maxItems", 256}}},
			  {"environment", {{"type", "object"}, {"additionalProperties", text}}}}},
			{"required", Json::array({"executable"})},
			{"additionalProperties", false}}}}});
	if (windowless)
		tools.push_back(
			{{"name", "simulator_shutdown"},
			 {"description", "Shut down this windowless simulator instance; launched applications keep running."},
			 {"inputSchema", {{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}}});
#ifdef SIMXR_COMPANION_AUTOMATION
	const Json integer = {
		{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
							   {{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	const Json seed = {
		{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,19})$"}},
							   {{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}})}};
	const auto luauTool = [](const char* name, const char* description, Json properties, Json required) {
		return Json{{"name", name},
					{"description", description},
					{"inputSchema",
					 {{"type", "object"},
					  {"properties", std::move(properties)},
					  {"required", std::move(required)},
					  {"additionalProperties", false}}}};
	};
	tools.push_back(luauTool(
		"simulator_luau_run",
		"Run bounded Luau source on one selected application's current XR session. Poll status for completion.",
		{{"application_id", identifier},
		 {"source", {{"type", "string"}, {"minLength", 1}, {"maxLength", 524288}}},
		 {"name", {{"type", "string"}, {"maxLength", 256}}},
		 {"seed", seed},
		 {"deadline_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600}, {"default", 30}}},
		 {"memory_mib", {{"type", "integer"}, {"minimum", 1}, {"maximum", 512}}}},
		Json::array({"application_id", "source"})));
	for (const auto& [name, description] :
		 {std::pair{"simulator_luau_status",
					"Read this MCP session's current or selected Luau run, including phase, output and checkpoints."},
		  {"simulator_luau_pause",
		   "Request a pause at the next simulator binding boundary; pure Luau code runs until then."},
		  {"simulator_luau_resume", "Resume this MCP session's paused Luau run."},
		  {"simulator_luau_step", "Advance this MCP session's paused Luau run by one binding boundary."},
		  {"simulator_luau_cancel", "Cancel this MCP session's Luau run and release its runtime resources."}})
		tools.push_back(luauTool(name, description, {{"application_id", identifier}, {"run_id", integer}},
								 name == std::string_view("simulator_luau_status")
									 ? Json::array({"application_id"})
									 : Json::array({"application_id", "run_id"})));
#endif
	for (auto& definition : McpDebug::Definitions())
	{
		if (definition.value("name", "") == "debug_export_info")
		{
			auto artifact = definition;
			artifact["name"] = "debug_export_artifact";
			artifact["description"] = "Create an authenticated, short-lived binary download link for a completed image "
									  "or video export. The export must stay retained.";
			tools.push_back(std::move(artifact));
		}
		tools.push_back(std::move(definition));
	}
	for (auto& definition : McpSequenceJobs::Definitions())
		tools.push_back(std::move(definition));
	return tools;
}
} // namespace simxr::app
