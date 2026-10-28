#include "app/mcp/McpPch.h"
#include "app/mcp/McpDiagnostics.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpTransientDebug.h"
#include "control/ControlSchema.h"

namespace simxr::app::mcp
{
using Json = nlohmann::json;
namespace wire = control;

static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}
bool DiagnosticsHandles(const std::string& name)
{
	static const std::set<std::string> names{"get_projection_log", "get_frame_info", "get_diagnostics", "diagnose_issue", "get_session_state", "get_head_tracking"};
	return names.contains(name);
}
Json DiagnosticsDefinitions()
{
	const Json common = {{"application_id", {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}}},
		{"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 30}}}};
	const auto tool = [](const char* name, const char* description, Json properties, Json required)
	{
		return Json{{"name", name}, {"description", description}, {"inputSchema", {{"type", "object"},
			{"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}}}};
	};
	auto projection = common;
	projection["max_entries"] = {{"type", "integer"}, {"minimum", 1}, {"maximum", 64}};
	auto issue = common;
	issue["symptoms"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 2048}};
	return Json::array({
		tool("get_projection_log", "Read the selected application's bounded submitted projection geometry without image readback.", projection, Json::array({"application_id"})),
		tool("get_frame_info", "Read the selected application's pipeline, recent projection and configured simulation state; samples can advance independently.", common, Json::array({"application_id"})),
		tool("get_diagnostics", "Read the selected application's runtime status, bounded diagnostic events and recent projection data. Native driver validation and text logs are not included.", common, Json::array({"application_id"})),
		tool("diagnose_issue", "Collect bounded runtime evidence and observed-state hints for the selected application. Hints are not proof of a cause.", issue, Json::array({"application_id", "symptoms"})),
		tool("get_session_state", "Read the selected application's actual OpenXR session state.", common, Json::array({"application_id"})),
		tool("get_head_tracking", "Read the selected application's configured simulator head pose; this is not an application-located tracking sample.", common, Json::array({"application_id"}))});
}
static std::uint64_t Capabilities(const Json& opened)
{
	const auto text = opened.at("capabilities").get<std::string>();
	std::uint64_t value = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
	if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
		throw std::runtime_error("Runtime returned invalid capability bits");
	return value;
}
static const char* SessionState(std::int64_t state)
{
	static constexpr std::array names{"unknown", "idle", "ready", "synchronized", "visible", "focused", "stopping", "loss_pending", "exiting"};
	return state >= 0 && static_cast<std::size_t>(state) < names.size() ? names[static_cast<std::size_t>(state)] : "unrecognized";
}
std::optional<Json> RunDiagnostics(McpApplication& application, const std::string& name, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	try
	{
		if (!arguments.is_object())
			throw std::invalid_argument("Arguments must be an object");
		for (auto it = arguments.begin(); it != arguments.end(); ++it)
			if (it.key() != "application_id" && it.key() != "timeout" &&
				!(name == "get_projection_log" && it.key() == "max_entries") && !(name == "diagnose_issue" && it.key() == "symptoms"))
				throw std::invalid_argument("Unknown argument: " + it.key());
		const auto& selected = arguments.at("application_id");
		if (!selected.is_string() || selected.get_ref<const std::string&>().empty() || selected.get_ref<const std::string&>().size() > 256)
			throw std::invalid_argument("application_id must identify a selected application");
		const auto timeout = arguments.value("timeout", 5.0);
		if (!std::isfinite(timeout) || timeout <= 0 || timeout > 30)
			throw std::invalid_argument("timeout must be greater than zero and at most 30 seconds");
		unsigned count = 20;
		if (name == "get_projection_log" && arguments.contains("max_entries"))
		{
			const auto& value = arguments.at("max_entries");
			if (!value.is_number_integer() || value < 1 || value > 64)
				throw std::invalid_argument("max_entries must be an integer from 1 to 64");
			count = value.get<unsigned>();
		}
		std::string symptoms;
		if (name == "diagnose_issue")
		{
			const auto& value = arguments.at("symptoms");
			if (!value.is_string() || value.get_ref<const std::string&>().empty() || value.get_ref<const std::string&>().size() > 2048)
				throw std::invalid_argument("symptoms must be a nonempty string of at most 2048 bytes");
			symptoms = value.get<std::string>();
		}
		if (!cancellation->TryAdmit())
			return {};
		TransientDebug calls(application, cancellation, timeout, "Diagnostics");
		const auto app = calls.Application("simulator_application_status", {{"application_id", selected}});
		Json result = {{"ok", true}, {"application_id", selected}, {"application", app},
			{"sampling", "Runtime, projection and simulation data are sampled separately and may advance between reads."}};
		if (name == "diagnose_issue")
			result["symptoms"] = symptoms;
		if (!app.value("connected", false) || !app.value("session_running", false))
		{
			result["runtime_available"] = false;
			result["unavailable"] = "No connected running XR session; runtime state was not sampled.";
			if (name == "get_projection_log" || name == "get_head_tracking" || name == "get_session_state" || name == "get_frame_info")
			{
				result["ok"] = false;
				return Result(result, true);
			}
			if (name == "diagnose_issue")
				result["observations"] = Json::array({"No connected running XR session is currently reported."});
			return Result(result);
		}
		const auto opened = calls.Data("debug_connect", {{"application_id", selected}});
		if (app.at("runtime_process_id") != opened.at("runtime_process_id") ||
			app.at("runtime_epoch") != opened.at("runtime_epoch") ||
			app.at("session_id") != opened.at("session") || app.at("generation") != opened.at("generation"))
			throw std::runtime_error("Application runtime or session changed during diagnostic collection; retry explicitly");
		const auto connection = opened.at("connection").get<std::string>();
		const Json common = {{"connection", connection}};
		const auto capabilities = Capabilities(opened);
		const auto supports = [&](wire::Capability value) { return (capabilities & static_cast<std::uint64_t>(value)) != 0; };
		result["runtime_available"] = true;
		result["session"] = opened.at("session");
		result["generation"] = opened.at("generation");
		result["capabilities"] = opened.at("capabilities");
		if (name != "get_projection_log" && name != "get_head_tracking")
		{
			const auto status = calls.Data("debug_status", common);
			result["runtime"] = status;
			if (status.contains("state") && status.at("state").is_number_integer())
				result["session_state"] = SessionState(status.at("state").get<std::int64_t>());
		}
		if (name == "get_projection_log" || name == "get_frame_info" || name == "get_diagnostics" || name == "diagnose_issue")
		{
			if (supports(wire::Capability::ProjectionHistory))
				result["projection"] = calls.Data("debug_projection_history", {{"connection", connection}, {"count", name == "get_projection_log" ? count : 1}}).at("document");
			else
				result["projection_unavailable"] = "The connected runtime does not advertise projection history.";
		}
		if (name == "get_head_tracking" || name == "get_frame_info" || name == "get_diagnostics" || name == "diagnose_issue")
		{
			if (supports(wire::Capability::SimulationCommands))
			{
				const auto simulation = calls.Data("simulation_status", common).at("document");
				if (name == "get_head_tracking")
					result["configured_head_pose"] = simulation.at("configuration").at("head");
				else
					result["simulation"] = simulation;
			}
			else
				result["simulation_unavailable"] = "The connected runtime does not advertise simulation commands.";
		}
		if (name == "get_diagnostics" || name == "diagnose_issue")
		{
			result["logs_unavailable"] = "Native driver validation and text logs are not included.";
			if (supports(wire::Capability::FrameDiagnostics))
			{
				const auto bounds = calls.Data("simulation_diagnostics", {{"connection", connection}, {"count", 0}}).at("document");
				const auto& cursor = bounds.at("next");
				const auto text = cursor.is_string() ? cursor.get<std::string>() :
					cursor.is_number_unsigned() ? std::to_string(cursor.get<std::uint64_t>()) : std::string{};
				std::uint64_t next = 0;
				const auto parsed = std::from_chars(text.data(), text.data() + text.size(), next);
				if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !next)
					throw std::runtime_error("Runtime returned invalid diagnostic cursor");
				const auto after = next > 65 ? next - 65 : 0;
				result["diagnostics"] = calls.Data("simulation_diagnostics", {{"connection", connection}, {"after", std::to_string(after)}, {"count", 64}}).at("document");
				result["diagnostic_cursor_sample"] = text;
			}
			else
				result["diagnostics_unavailable"] = "The connected runtime does not advertise frame diagnostics.";
		}
		if (name == "diagnose_issue")
		{
			Json observations = Json::array();
			const auto& status = result.at("runtime");
			if (status.contains("pipeline") && status.at("pipeline").value("paused", false))
				observations.push_back("The frame pipeline is paused; frame advancement requires an explicit step or resume.");
			if (result.contains("projection") && result.at("projection").contains("entries") && result.at("projection").at("entries").empty())
				observations.push_back("No projection entries are present in the sampled history.");
			if (observations.empty())
				observations.push_back("No specific cause is established by these samples; inspect the returned events and projection data.");
			result["observations"] = std::move(observations);
		}
		calls.Check();
		return Result(result);
	}
	catch (const std::exception& error)
	{
		if (cancellation->Requested())
			return {};
		return Result({{"ok", false}, {"error", error.what()}}, true);
	}
}
}
