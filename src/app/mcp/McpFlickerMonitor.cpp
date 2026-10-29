#include "app/mcp/McpPch.h"
#include "app/mcp/McpFlickerMonitor.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpFlickerCoverage.h"
#include "app/mcp/McpIncident.h"

namespace simxr::app
{
using Json = nlohmann::json;
static std::atomic<unsigned> s_flickerMonitors{0};
class FlickerMonitorPermit
{
  public:
	FlickerMonitorPermit()
	{
		auto count = s_flickerMonitors.load();
		do
		{
			if (count >= 2)
				throw std::runtime_error("Flicker monitor capacity is busy; retry explicitly");
		} while (!s_flickerMonitors.compare_exchange_weak(count, count + 1));
	}
	~FlickerMonitorPermit() { if (m_owned) --s_flickerMonitors; }
	void Transfer() { m_owned = false; }
  private:
	bool m_owned = true;
};

static Json Result(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}

Json McpFlickerMonitor::Definitions()
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
bool McpFlickerMonitor::Handles(const std::string& name)
{
	return name == "start_flicker_monitor" || name == "get_flicker_status" ||
		name == "get_ui_flicker_status" || name == "stop_flicker_monitor";
}

static std::string Text(const Json& arguments, const char* key, std::size_t maximum)
{
	const auto& value = arguments.at(key);
	if (!value.is_string() || value.get_ref<const std::string&>().empty() ||
		value.get_ref<const std::string&>().size() > maximum || value.get_ref<const std::string&>().find('\0') != std::string::npos)
		throw std::invalid_argument(std::string("Invalid ") + key);
	return value.get<std::string>();
}

std::optional<Json> McpFlickerMonitor::Call(McpApplication& app, const std::string& name, const Json& arguments,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	try
	{
		if (!arguments.is_object())
			throw std::invalid_argument("Arguments must be an object");
		if (name == "get_flicker_status" || name == "get_ui_flicker_status")
		{
			if (!arguments.empty())
				throw std::invalid_argument("Status takes no arguments");
			std::lock_guard lock(m_mutex);
			const auto target = name == "get_ui_flicker_status" ? "ui" : "composed";
			if (m_status.is_null() || m_status.at("target") != target)
				return Result({{"ok", true}, {"phase", "not_started"}, {"target", target}});
			return Result(m_status);
		}
		if (name == "stop_flicker_monitor")
		{
			if (arguments.size() != 1)
				throw std::invalid_argument("Supply monitor_id only");
			const auto id = Text(arguments, "monitor_id", 128);
			if (!cancellation->TryAdmit())
				return {};
			std::lock_guard lifecycle(m_lifecycle);
			{
				std::lock_guard lock(m_mutex);
				if (m_status.is_null() || m_status.at("monitor_id") != id)
					throw std::invalid_argument("Unknown monitor_id for this application");
				if (m_cancellation)
					m_cancellation->Cancel();
			}
			m_wake.notify_all();
			if (m_worker.joinable())
				m_worker.join();
			std::lock_guard lock(m_mutex);
			return Result(m_status);
		}
		if (name != "start_flicker_monitor")
			throw std::invalid_argument("Unknown monitor tool");
		for (auto item = arguments.begin(); item != arguments.end(); ++item)
			if (item.key() != "monitor_id" && item.key() != "application_id" && item.key() != "target" &&
				item.key() != "interval" && item.key() != "timeout")
				throw std::invalid_argument("Unknown argument: " + item.key());
		const auto id = Text(arguments, "monitor_id", 128);
		const auto selected = Text(arguments, "application_id", 256);
		const auto target = Text(arguments, "target", 16);
		if (target != "composed" && target != "ui")
			throw std::invalid_argument("target must be composed or ui");
		const double interval = arguments.value("interval", .25), timeout = arguments.value("timeout", 10.0);
		if (!std::isfinite(interval) || interval < .25 || interval > 30 || !std::isfinite(timeout) || timeout <= 0 || timeout > 30)
			throw std::invalid_argument("Invalid monitor interval or timeout");
		const Json options = {{"monitor_id", id}, {"application_id", selected}, {"target", target},
			{"interval", interval}, {"timeout", timeout}};
		if (!cancellation->TryAdmit())
			return {};
		std::lock_guard lifecycle(m_lifecycle);
		{
			std::lock_guard lock(m_mutex);
			if (m_closed)
				throw std::runtime_error("Monitor owner is closed");
			if (!m_status.is_null() && m_status.at("monitor_id") == id)
			{
				if (m_options != options)
					throw std::invalid_argument("monitor_id was already used with different options");
				return Result(m_status);
			}
			if (!m_status.is_null() && m_status.at("phase") == "running")
				throw std::runtime_error("A monitor is already running for this application");
		}
		if (m_worker.joinable())
			m_worker.join();
		auto workCancellation = std::make_shared<McpCancellation>();
		FlickerMonitorPermit permit;
		{
			std::lock_guard lock(m_mutex);
			m_options = options;
			m_cancellation = workCancellation;
			m_status = {{"ok", true}, {"monitor_id", id}, {"application_id", selected}, {"target", target},
				{"phase", "running"}, {"windows", 0}, {"frames_observed", 0}, {"unobserved_frames", 0},
				{"frames_skipped", 0}, {"events", Json::array()}, {"events_total", 0}, {"events_dropped", 0},
				{"coverage", "sampled_windows"}};
		}
		try
		{
			m_worker = std::thread(&McpFlickerMonitor::Run, this, std::ref(app), options, workCancellation);
			permit.Transfer();
		}
		catch (...)
		{
			std::lock_guard lock(m_mutex);
			m_status["phase"] = "failed";
			m_status["ok"] = false;
			m_status["error"] = "Could not start monitor worker";
			throw;
		}
		std::lock_guard lock(m_mutex);
		return Result(m_status);
	}
	catch (const std::exception& error)
	{
		if (cancellation->Requested())
			return {};
		return Result({{"ok", false}, {"error", error.what()}}, true);
	}
}

void McpFlickerMonitor::Run(McpApplication& app, Json options, std::shared_ptr<McpCancellation> cancellation) noexcept
{
	struct Permit { ~Permit() { --s_flickerMonitors; } } permit;
	try
	{
		mcp::FlickerCoverage coverage;
		Json identity;
		const mcp::IncidentRequest request{.application = options.at("application_id").get<std::string>(),
			.timeout = options.at("timeout").get<double>(), .maximum = 2, .ui = options.at("target") == "ui",
			.analyze = true, .includeImage = false, .allowMissing = true};
		const auto interval = std::chrono::duration<double>(options.at("interval").get<double>());
		while (!cancellation->Requested())
		{
			const auto response = mcp::CaptureIncident(app, request, cancellation);
			if (!response || cancellation->Requested())
				break;
			const auto& document = response->metadata;
			const Json currentIdentity = {document.at("runtime_process_id"), document.at("runtime_epoch"),
				document.at("session"), document.at("generation")};
			if (!identity.is_null() && identity != currentIdentity)
				throw std::runtime_error("The selected XR session changed; start a new monitor explicitly");
			identity = currentIdentity;
			coverage.AddWindow(document.at("window").at("entries"), document.at("analysis"));
			const auto counts = coverage.Report();
			{
				std::lock_guard lock(m_mutex);
				m_status["windows"] = m_status.at("windows").get<std::uint64_t>() + 1;
				for (auto item = counts.begin(); item != counts.end(); ++item)
					m_status[item.key()] = item.value();
				m_status["runtime_process_id"] = identity[0];
				m_status["runtime_epoch"] = identity[1];
				m_status["session"] = identity[2];
				m_status["generation"] = identity[3];
				m_status["last_analysis"] = document.at("analysis");
			}
			std::unique_lock lock(m_mutex);
			m_wake.wait_for(lock, interval,
				[&] { return cancellation->Requested(); });
		}
		std::lock_guard lock(m_mutex);
		m_status["phase"] = "stopped";
	}
	catch (const std::exception& error)
	{
		std::lock_guard lock(m_mutex);
		m_status["phase"] = cancellation->Requested() ? "stopped" : "failed";
		if (!cancellation->Requested())
		{
			m_status["ok"] = false;
			m_status["error"] = error.what();
		}
	}
	catch (...)
	{
		std::lock_guard lock(m_mutex);
		m_status["phase"] = "failed";
		m_status["ok"] = false;
		m_status["error"] = "Unexpected flicker monitor failure";
	}
}

void McpFlickerMonitor::Close() noexcept
{
	std::lock_guard lifecycle(m_lifecycle);
	{
		std::lock_guard lock(m_mutex);
		m_closed = true;
		if (m_cancellation)
			m_cancellation->Cancel();
	}
	m_wake.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}
McpFlickerMonitor::~McpFlickerMonitor()
{
	Close();
}
}
