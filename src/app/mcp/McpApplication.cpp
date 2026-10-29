#include "app/UiPch.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpFlickerMonitor.h"
#include "app/services/CompanionSettings.h"
#include "app/services/Utf8Path.h"
#include <exception>

namespace simxr::app
{
using Json = nlohmann::json;

static Json ToolResult(const Json& value, bool error = false)
{
	return {{"content", Json::array({{{"type", "text"}, {"text", value.dump()}}})}, {"isError", error}};
}

static Json Error(const std::string& message)
{
	return ToolResult({{"error", message}}, true);
}

static const char* State(LaunchState state)
{
	switch (state)
	{
	case LaunchState::Idle: return "idle";
	case LaunchState::Starting: return "starting";
	case LaunchState::Running: return "running";
	case LaunchState::Exited: return "exited";
	case LaunchState::Failed: return "failed";
	}
	return "unknown";
}

static Json Application(const ApplicationSnapshot& app, const std::string& instanceId)
{
	return {{"application_id", instanceId + ":" + std::to_string(app.id)}, {"name", app.name},
			{"launch_state", State(app.launch.state)}, {"launch_detail", app.launch.detail},
			{"restart_pending", app.launch.restartPending}, {"launch_error", app.error},
			{"process_id", std::to_string(app.launch.process)}, {"runtime_process_id", std::to_string(app.runtimeProcess)},
			{"runtime_epoch", std::to_string(app.runtimeEpoch)}, {"exit_code", app.launch.exitCode},
			{"endpoint", app.endpoint}, {"launch_endpoint", app.launchEndpoint}, {"connected", app.connected}, {"session_running", app.running},
			{"inferred_runtime_data_directory", app.launchDataDirectory.empty() ? Json(nullptr) : Json(app.launchDataDirectory)},
			{"session_id", std::to_string(app.session)}, {"generation", std::to_string(app.generation)}};
}

static void CheckKeys(const Json& object, const std::set<std::string>& allowed)
{
	if (!object.is_object())
		throw std::invalid_argument("Arguments must be an object");
	for (auto item = object.begin(); item != object.end(); ++item)
		if (!allowed.contains(item.key()))
			throw std::invalid_argument("Unknown argument: " + item.key());
}

static std::string Text(const Json& object, const char* key, bool required = false)
{
	const auto found = object.find(key);
	if (found == object.end())
	{
		if (required)
			throw std::invalid_argument(std::string("Missing argument: ") + key);
		return {};
	}
	if (!found->is_string())
		throw std::invalid_argument(std::string(key) + " must be a string");
	const auto value = found->get<std::string>();
	if (value.size() > 1024 * 1024 || value.find('\0') != std::string::npos || (required && value.empty()))
		throw std::invalid_argument(std::string("Invalid argument: ") + key);
	return value;
}

static std::uint64_t Identifier(const std::string& value, const std::string& instanceId)
{
	const auto prefix = instanceId + ":";
	if (!value.starts_with(prefix))
		throw std::invalid_argument("application_id belongs to a different simulator instance");
	const auto number = std::string_view(value).substr(prefix.size());
	std::uint64_t result = 0;
	const auto parsed = std::from_chars(number.data(), number.data() + number.size(), result);
	if (number.empty() || number.front() == '0' || parsed.ec != std::errc{} ||
		parsed.ptr != number.data() + number.size() || !result)
		throw std::invalid_argument("application_id contains an invalid application number");
	return result;
}

static std::uint64_t ProcessIdentifier(const std::string& value)
{
	std::uint64_t result = 0;
	const auto parsed = std::from_chars(value.data(), value.data() + value.size(), result);
	if (value.empty() || value.front() == '0' || parsed.ec != std::errc{} ||
		parsed.ptr != value.data() + value.size() || !result)
		throw std::invalid_argument("process_id must be a nonzero canonical decimal string");
	return result;
}

static Json Execute(ApplicationCatalog& tabs, const std::string& instanceId, const std::string& name, const Json& arguments,
					const std::shared_ptr<AutomationOwner>& owner)
{
#ifdef SIMXR_COMPANION_AUTOMATION
	if (name == "simulator_luau_run" || name == "simulator_luau_status" || name == "simulator_luau_pause" ||
		name == "simulator_luau_resume" || name == "simulator_luau_step" || name == "simulator_luau_cancel")
	{
		if (!owner || !owner->active.load())
			throw std::runtime_error("The MCP automation session is closed");
		const auto number = [&](const char* key, bool allowZero, std::uint64_t fallback)
		{
			const auto found = arguments.find(key);
			if (found == arguments.end())
				return fallback;
			std::uint64_t value = 0;
			if (found->is_string())
			{
				const auto text = found->get<std::string>();
				if (text.empty() || (text.size() > 1 && text.front() == '0'))
					throw std::invalid_argument(std::string(key) + " requires a canonical decimal integer");
				const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
				if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
					throw std::invalid_argument(std::string(key) + " requires a canonical decimal integer");
			}
			else if (found->is_number_integer() && *found >= 0 && *found <= ((1ULL << 53) - 1))
				value = found->get<std::uint64_t>();
			else
				throw std::invalid_argument(std::string(key) + " requires a decimal string or safe integer");
			if (!allowZero && !value)
				throw std::invalid_argument(std::string(key) + " must be nonzero");
			return value;
		};
		const auto application = Identifier(Text(arguments, "application_id", true), instanceId);
		if (name == "simulator_luau_run")
		{
			CheckKeys(arguments, {"application_id", "source", "name", "seed", "deadline_seconds", "memory_mib"});
			LuauRunRequest request;
			request.source = Text(arguments, "source", true);
			request.name = Text(arguments, "name");
			if (request.name.empty())
				request.name = "mcp.luau";
			request.seed = number("seed", true, 0);
			const auto deadline = number("deadline_seconds", false, 30);
			const auto memory = number("memory_mib", false, 64);
			if (deadline > 3600 || memory > 512 ||
				(arguments.contains("deadline_seconds") && !arguments["deadline_seconds"].is_number_integer()) ||
				(arguments.contains("memory_mib") && !arguments["memory_mib"].is_number_integer()))
				throw std::invalid_argument("Luau deadline or memory limit is out of range");
			request.deadlineSeconds = static_cast<int>(deadline);
			request.memoryMiB = static_cast<int>(memory);
			const auto run = tabs.StartLuau(application, std::move(request), owner);
			return ToolResult({{"application_id", instanceId + ":" + std::to_string(application)},
				{"run_id", std::to_string(run)}, {"accepted", true}});
		}
		CheckKeys(arguments, {"application_id", "run_id"});
		const auto run = number("run_id", false, 0);
		if (!run && name != "simulator_luau_status")
			throw std::invalid_argument("run_id is required");
		if (name == "simulator_luau_status")
		{
			const auto status = tabs.LuauStatus(application, run, owner);
			const auto& snapshot = status.snapshot;
			const char* phase = "idle";
			switch (snapshot.phase)
			{
			case automation::RigPhase::Idle: phase = "idle"; break;
			case automation::RigPhase::Running: phase = "running"; break;
			case automation::RigPhase::Paused: phase = "paused"; break;
			case automation::RigPhase::Complete: phase = "complete"; break;
			case automation::RigPhase::Cancelled: phase = "cancelled"; break;
			case automation::RigPhase::Failed: phase = "failed"; break;
			}
			Json checkpoints = Json::array();
			for (const auto& checkpoint : snapshot.checkpoints)
				checkpoints.push_back({{"name", checkpoint.name}, {"detail", checkpoint.detail}, {"passed", checkpoint.passed}});
			Json result = {{"run_id", std::to_string(status.run)}, {"phase", phase}, {"seed", std::to_string(snapshot.seed)},
				{"transactions", std::to_string(snapshot.transactions)}, {"captures", std::to_string(snapshot.captures)},
				{"error", snapshot.error}, {"checkpoints", std::move(checkpoints)}, {"output", snapshot.output}};
			if (snapshot.compatibilityDeviations)
				result["compatibility_deviations"] = *snapshot.compatibilityDeviations;
			return ToolResult(result);
		}
		const auto action = name == "simulator_luau_pause" ? LuauControl::Pause :
			name == "simulator_luau_resume" ? LuauControl::Resume :
			name == "simulator_luau_step" ? LuauControl::Step : LuauControl::Cancel;
		tabs.ControlLuau(application, run, action, owner);
		return ToolResult({{"run_id", std::to_string(run)}, {"accepted", true}});
	}
#else
	(void)owner;
#endif
	if (name == "simulator_shutdown")
	{
		CheckKeys(arguments, {});
		if (!tabs.RequestShutdown())
			return Error("Shutdown is available only in windowless mode");
		return ToolResult({{"accepted", true}});
	}
	if (name == "simulator_list_applications")
	{
		CheckKeys(arguments, {});
		Json applications = Json::array();
		for (const auto& app : tabs.Applications())
			applications.push_back(Application(app, instanceId));
		return ToolResult({{"applications", std::move(applications)}});
	}
	if (name == "simulator_stop_application")
	{
		CheckKeys(arguments, {"application_id"});
		const auto id = Identifier(Text(arguments, "application_id", true), instanceId);
		tabs.RequestApplicationClose(id);
		return ToolResult({{"application_id", instanceId + ":" + std::to_string(id)}, {"accepted", true}});
	}
	if (name == "simulator_restart_application")
	{
		CheckKeys(arguments, {"application_id", "process_id"});
		const auto id = Identifier(Text(arguments, "application_id", true), instanceId);
		const auto process = ProcessIdentifier(Text(arguments, "process_id", true));
		tabs.RequestApplicationRestart(id, process);
		return ToolResult({{"application_id", instanceId + ":" + std::to_string(id)},
						   {"process_id", std::to_string(process)}, {"accepted", true}});
	}
	if (name == "simulator_launch_application")
	{
		CheckKeys(arguments, {"executable", "name", "arguments", "working_directory", "runtime_manifest", "environment"});
		LaunchProfile profile;
		profile.executable = Text(arguments, "executable", true);
		const auto executable = Utf8Path(profile.executable);
		if (!executable.is_absolute())
			throw std::invalid_argument("Executable must be an absolute path");
		if (const auto problem = PathProblem(profile.executable, PathKind::Executable); !problem.empty())
			throw std::invalid_argument("Executable: " + problem);
		profile.name = Text(arguments, "name");
		if (profile.name.empty())
			profile.name = PathText(executable.stem());
		profile.workingDirectory = Text(arguments, "working_directory");
		if (profile.workingDirectory.empty())
			profile.workingDirectory = PathText(executable.parent_path());
		if (!Utf8Path(profile.workingDirectory).is_absolute() ||
			!PathProblem(profile.workingDirectory, PathKind::Folder).empty())
			throw std::invalid_argument("Working directory must be an existing absolute folder");
		profile.runtimeManifest = Text(arguments, "runtime_manifest");
		profile.useBundledRuntime = profile.runtimeManifest.empty();
		if (!profile.useBundledRuntime &&
			(!Utf8Path(profile.runtimeManifest).is_absolute() ||
			 !PathProblem(profile.runtimeManifest, PathKind::File).empty()))
			throw std::invalid_argument("Runtime manifest must be an existing absolute file");
		if (const auto found = arguments.find("arguments"); found != arguments.end())
		{
			if (!found->is_array() || found->size() > 256)
				throw std::invalid_argument("arguments must be an array of at most 256 strings");
			for (const auto& argument : *found)
			{
				if (!argument.is_string())
					throw std::invalid_argument("Every application argument must be a string");
				profile.arguments.push_back(argument.get<std::string>());
			}
		}
		if (const auto found = arguments.find("environment"); found != arguments.end())
		{
			if (!found->is_object() || found->size() > MaxLaunchEnvironment)
				throw std::invalid_argument("environment must be an object of strings");
			for (auto item = found->begin(); item != found->end(); ++item)
			{
				if (!item.value().is_string())
					throw std::invalid_argument("Every environment value must be a string");
				profile.environment.emplace_back(item.key(), item.value().get<std::string>());
			}
		}
		ValidateLaunchProfile(profile);
		const auto id = tabs.LaunchApplication(profile);
		return ToolResult({{"application_id", instanceId + ":" + std::to_string(id)}, {"accepted", true}});
	}
	return Error("Unknown tool: " + name);
}

struct McpApplication::Request
{
	std::string name;
	Json arguments, result;
	std::uint64_t application = 0;
	ApplicationSnapshot snapshot;
	std::exception_ptr error;
	std::shared_ptr<McpCancellation> cancellation;
	std::shared_ptr<AutomationOwner> owner;
};

void McpCancellation::Cancel() noexcept
{
	std::lock_guard lock(m_mutex);
	m_requested = true;
	if (m_state == State::Queued)
	{
		m_state = State::Cancelled;
		m_changed.notify_all();
	}
}

bool McpCancellation::Requested() noexcept
{
	std::lock_guard lock(m_mutex);
	return m_requested;
}

bool McpCancellation::TryAdmit() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_requested || m_state == State::Cancelled)
		return false;
	m_state = State::Running;
	return true;
}

McpApplication::~McpApplication()
{
	Close();
}

McpApplication::McpApplication(bool windowless) : m_instanceId(NewMcpIdentifier()), m_windowless(windowless) {}

bool McpApplication::Windowless() const noexcept
{
	return m_windowless;
}

std::optional<Json> McpApplication::Call(std::string name, Json arguments,
										const std::shared_ptr<McpCancellation>& cancellation,
										std::shared_ptr<AutomationOwner> owner)
{
	try
	{
		if (name == "simulator_application_status")
		{
			CheckKeys(arguments, {"application_id"});
			const auto snapshot = Lookup(Text(arguments, "application_id", true), cancellation);
			return snapshot ? std::optional{ToolResult(ApplicationDocument(*snapshot))} : std::nullopt;
		}
		auto request = std::make_shared<Request>();
		request->name = std::move(name);
		request->arguments = std::move(arguments);
		request->cancellation = cancellation;
		request->owner = std::move(owner);
		if (!Wait(request))
			return {};
		return std::move(request->result);
	}
	catch (const std::exception& error)
	{
		return Error(error.what());
	}
}

std::optional<ApplicationSnapshot> McpApplication::Lookup(const std::string& application,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	auto request = std::make_shared<Request>();
	request->application = Identifier(application, m_instanceId);
	request->cancellation = cancellation;
	if (!Wait(request))
		return {};
	return std::move(request->snapshot);
}

Json McpApplication::ApplicationDocument(const ApplicationSnapshot& application) const
{
	return Application(application, m_instanceId);
}

bool McpApplication::Wait(const std::shared_ptr<Request>& request)
{
	{
		std::lock_guard lock(m_mutex);
		if (m_closed)
			throw std::runtime_error("OpenXR Simulator is shutting down");
		if (m_pending.size() >= 32)
			throw std::runtime_error("OpenXR Simulator request queue is full");
		m_pending.push_back(request);
	}
	const auto& cancellation = request->cancellation;
	std::unique_lock lock(cancellation->m_mutex);
	if (cancellation->m_changed.wait_for(lock, std::chrono::seconds(5), [&]
		{ return cancellation->m_state == McpCancellation::State::Complete ||
				 cancellation->m_state == McpCancellation::State::Cancelled; }))
	{
		if (cancellation->m_state == McpCancellation::State::Cancelled)
			return false;
		if (request->error)
			std::rethrow_exception(request->error);
		return true;
	}
	if (cancellation->m_state == McpCancellation::State::Queued)
	{
		cancellation->m_state = McpCancellation::State::Cancelled;
		throw std::runtime_error("The application did not admit the request before its deadline");
	}
	throw std::runtime_error("The request timed out after admission; its outcome is unknown. Inspect applications before retrying.");
}

void McpApplication::Poll(ApplicationCatalog& tabs)
{
	for (unsigned index = 0; index < 8; ++index)
	{
		std::shared_ptr<Request> request;
		{
			std::lock_guard lock(m_mutex);
			if (m_pending.empty())
				return;
			request = std::move(m_pending.front());
			m_pending.pop_front();
		}
		{
			std::lock_guard lock(request->cancellation->m_mutex);
			if (request->cancellation->m_state != McpCancellation::State::Queued)
				continue;
			request->cancellation->m_state = McpCancellation::State::Running;
		}
		try
		{
			if (request->application)
			{
				auto applications = tabs.Applications();
				const auto found = std::find_if(applications.begin(), applications.end(),
					[&](const auto& app) { return app.id == request->application; });
				if (found == applications.end())
					throw std::runtime_error("Application is no longer available");
				request->snapshot = std::move(*found);
			}
			else
				request->result = Execute(tabs, m_instanceId, request->name, request->arguments, request->owner);
		}
		catch (...)
		{
			request->error = std::current_exception();
		}
		{
			std::lock_guard lock(request->cancellation->m_mutex);
			request->cancellation->m_state = McpCancellation::State::Complete;
		}
		request->cancellation->m_changed.notify_one();
	}
}

std::optional<Json> McpApplication::MonitorCall(const std::string& name, Json arguments,
	const std::shared_ptr<McpCancellation>& cancellation)
{
	try
	{
		if (!arguments.is_object())
			throw std::invalid_argument("Arguments must be an object");
		const auto selected = Text(arguments, "application_id", true);
		Identifier(selected, m_instanceId);
		const bool start = name == "start_flicker_monitor";
		if (start && !Lookup(selected, cancellation))
			return {};
		std::shared_ptr<McpFlickerMonitor> monitor;
		{
			std::lock_guard lock(m_mutex);
			if (m_closed)
				throw std::runtime_error("OpenXR Simulator is shutting down");
			const auto found = m_monitors.find(selected);
			if (found != m_monitors.end())
				monitor = found->second;
			else if (start)
			{
				if (m_monitors.size() >= 64)
					throw std::runtime_error("Monitored application capacity exhausted");
				monitor = std::make_shared<McpFlickerMonitor>();
				m_monitors.emplace(selected, monitor);
			}
		}
		if (!start)
			arguments.erase("application_id");
		if (!monitor)
		{
			if ((name == "get_flicker_status" || name == "get_ui_flicker_status") && arguments.empty())
				return ToolResult({{"ok", true}, {"phase", "not_started"},
					{"target", name == "get_ui_flicker_status" ? "ui" : "composed"}});
			throw std::invalid_argument("No monitor exists for this application");
		}
		return monitor->Call(*this, name, arguments, cancellation);
	}
	catch (const std::exception& error)
	{
		if (cancellation->Requested())
			return {};
		return Error(error.what());
	}
}

void McpApplication::Close() noexcept
{
	std::deque<std::shared_ptr<Request>> pending;
	decltype(m_monitors) monitors;
	{
		std::lock_guard lock(m_mutex);
		m_closed = true;
		pending.swap(m_pending);
		monitors.swap(m_monitors);
	}
	for (const auto& request : pending)
	{
		std::lock_guard lock(request->cancellation->m_mutex);
		if (request->cancellation->m_state == McpCancellation::State::Cancelled)
			continue;
		try { throw std::runtime_error("OpenXR Simulator is shutting down"); }
		catch (...) { request->error = std::current_exception(); }
		request->cancellation->m_state = McpCancellation::State::Complete;
		request->cancellation->m_changed.notify_one();
	}
	for (const auto& [application, monitor] : monitors)
		monitor->Close();
}
} // namespace simxr::app
