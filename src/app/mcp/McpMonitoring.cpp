#include "app/mcp/McpPch.h"
#include "app/mcp/McpMonitoring.h"
#include "app/services/ApplicationCatalog.h"
#include "app/services/MonitorController.h"

namespace simxr::app
{
using Json = nlohmann::json;
Json MonitorDefinitions()
{
	const auto tool = [](const char* name, const char* description, Json properties, Json required)
	{
		properties["application_id"] = {{"type", "string"}, {"minLength", 1}, {"maxLength", 256}};
		if (std::find(required.begin(), required.end(), "application_id") == required.end())
			required.push_back("application_id");
		return Json{{"name", name}, {"description", description}, {"inputSchema", {{"type", "object"},
			{"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}}}};
	};
	const Json id = {{"type", "string"}, {"minLength", 1}, {"maxLength", 128}};
	return Json::array({
		tool("start_flicker_monitor", "Start an application-owned, explicitly enabled sampled flicker monitor. It reads reduced frames; gaps and failures are reported. Reuse the same monitor_id only for an identical retry.",
			{{"monitor_id", id},
			 {"target", {{"type", "string"}, {"enum", Json::array({"composed", "ui"})}}},
			 {"interval", {{"type", "number"}, {"minimum", .25}, {"maximum", 30}}},
			 {"timeout", {{"type", "number"}, {"exclusiveMinimum", 0}, {"maximum", 30}}}},
			Json::array({"monitor_id", "target"})),
		tool("get_flicker_status", "Read the selected application's sampled composed-image monitor. Empty or interrupted coverage does not prove that flicker is absent.", Json::object(), Json::array()),
		tool("get_ui_flicker_status", "Read the selected application's sampled UI monitor. Missing eye images and unsampled frame gaps are explicit.", Json::object(), Json::array()),
		tool("stop_flicker_monitor", "Stop and join the selected application's monitor; its last status remains available until another monitor starts.",
			{{"monitor_id", id}}, Json::array({"monitor_id"}))});
}
bool HandlesMonitor(const std::string& name)
{
	return name == "start_flicker_monitor" || name == "get_flicker_status" ||
		name == "get_ui_flicker_status" || name == "stop_flicker_monitor";
}


static Json Report(const MonitorStatus& status, const std::string& target)
{
	if (!status.session || status.options.target != target)
		return {{"ok", true}, {"phase", "not_started"}, {"target", target}};
	const auto& report = status.run.monitor;
	const auto phase = status.run.phase;
	const bool failed = phase == automation::RigPhase::Failed;
	Json events = Json::array();
	for (const auto& event : report.events)
		events.push_back({{"frame_id", std::to_string(event.frame)}, {"reason", event.reason}});
	Json result = {{"ok", !failed}, {"monitor_id", status.options.id}, {"target", target},
		{"phase", failed ? "failed" : phase == automation::RigPhase::Running ? "running" : "stopped"},
		{"runtime_process_id", std::to_string(status.process)}, {"runtime_epoch", std::to_string(status.epoch)},
		{"session", std::to_string(status.session)}, {"generation", std::to_string(status.generation)},
		{"interval", status.options.interval}, {"timeout", status.options.timeout},
		{"coverage", "sampled_history"}, {"windows", report.windows}, {"frames_observed", report.observed},
		{"frames_skipped", report.skipped}, {"unobserved_frames", report.gaps},
		{"last_frame", report.lastFrame ? Json(std::to_string(report.lastFrame)) : Json(nullptr)},
		{"missed_cadences", report.missedCadences}, {"events_total", report.eventsTotal},
		{"events_dropped", report.eventsDropped}, {"events", std::move(events)},
		{"last_analysis", {{"frames_analyzed", report.lastAnalyzed}, {"frames_skipped", report.lastSkipped},
			{"temporal_pairs", report.lastPairs}, {"frame_gaps", report.lastGaps}}}};
	if (failed)
		result["error"] = status.run.error;
	return result;
}
Json CallMonitor(ApplicationCatalog& applications, std::uint64_t application,
	const std::string& name, const Json& arguments)
{
	const bool start = name == "start_flicker_monitor", stop = name == "stop_flicker_monitor";
	for (auto item = arguments.begin(); item != arguments.end(); ++item)
		if (item.key() != "application_id" && !((start || stop) && item.key() == "monitor_id") &&
			!(start && (item.key() == "target" || item.key() == "interval" || item.key() == "timeout")))
			throw std::invalid_argument("Unknown argument: " + item.key());
	if (start)
		applications.StartMonitor(application, {arguments.at("monitor_id").get<std::string>(),
			arguments.at("target").get<std::string>(), arguments.value("interval", .25), arguments.value("timeout", 10.0)});
	if (stop)
		applications.StopMonitor(application, arguments.at("monitor_id").get<std::string>());
	const auto status = applications.ReadMonitor(application);
	auto result = Report(status, start || stop ? status.options.target : name == "get_ui_flicker_status" ? "ui" : "composed");
	result["application_id"] = arguments.at("application_id");
	return result;
}
}
