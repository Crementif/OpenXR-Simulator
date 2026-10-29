#include "app/mcp/McpPch.h"
#include "app/mcp/McpServer.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpDebug.h"
#include "app/mcp/McpMonitoring.h"
#include "app/mcp/McpSequenceJobs.h"
#include "app/mcp/McpSocket.h"
#include "app/services/AutomationOwner.h"
#include "control/ProtocolTypes.h"
#include "simxr/Version.h"

#include <unordered_set>

namespace simxr::app
{
using Json = nlohmann::json;

struct McpServer::Session
{
	struct Call
	{
		std::shared_ptr<McpCancellation> cancellation;
		std::string key;
	};
	std::string version;
	std::chrono::steady_clock::time_point lastUsed = std::chrono::steady_clock::now();
	std::unordered_map<std::string, Call> calls;
	// Connector keys distinguish an early cancellation from a later reuse of the JSON-RPC ID.
	std::unordered_set<std::string> earlyCancellations;
	McpDebug debug;
	McpSequenceJobs sequenceJobs;
	std::shared_ptr<AutomationOwner> automationOwner = std::make_shared<AutomationOwner>();
	std::uint64_t artifactRevision = 0;
	bool initialized = false;
	~Session()
	{
		automationOwner->active.store(false);
	}
};

struct McpServer::Artifact
{
	std::string session, connection, mediaType;
	std::uint64_t ticket = 0;
	std::uint64_t size = 0;
	std::chrono::steady_clock::time_point expires;
};

struct McpDownloadSlot
{
	std::atomic<unsigned>& active;
	~McpDownloadSlot()
	{
		--active;
	}
};

static bool EqualToken(std::string_view supplied, std::string_view expected)
{
	if (supplied.size() != expected.size())
		return false;
	unsigned difference = 0;
	for (std::size_t index = 0; index < expected.size(); ++index)
		difference |= static_cast<unsigned char>(supplied[index]) ^ static_cast<unsigned char>(expected[index]);
	return difference == 0;
}

static bool SupportedVersion(std::string_view version)
{
	return version == "2025-03-26" || version == "2025-06-18" || version == "2025-11-25";
}

static bool ValidCancellationKey(std::string_view key)
{
	return key.size() == 64 && std::ranges::all_of(key, [](unsigned char value)
		{ return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); });
}

static Json RpcError(const Json& id, int code, const std::string& message)
{
	return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

static Json Tools(bool windowless)
{
	const Json text = {{"type", "string"}};
	const Json identifier = {{"type", "string"}, {"pattern", "^[0-9a-f]{64}:[1-9][0-9]{0,19}$"}};
	Json tools = Json::array({
		{{"name", "simulator_list_applications"},
		 {"description", "List applications in OpenXR Simulator, including their launch and connection state."},
		 {"inputSchema", {{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}}},
		{{"name", "simulator_application_status"},
		 {"description", "Read one application's launch, runtime connection and session state."},
		 {"inputSchema", {{"type", "object"}, {"properties", {{"application_id", identifier}}},
						  {"required", Json::array({"application_id"})}, {"additionalProperties", false}}}},
		{{"name", "simulator_stop_application"},
		 {"description", "Request a graceful close of an application launched by the simulator. "
						 "Use simulator_application_status to observe whether it exits; attached external applications are not supported."},
		 {"inputSchema", {{"type", "object"}, {"properties", {{"application_id", identifier}}},
						  {"required", Json::array({"application_id"})}, {"additionalProperties", false}}}},
		{{"name", "simulator_restart_application"},
		 {"description", "Request a graceful restart using the app's original launch profile. "
						 "Pass the current process_id from simulator_application_status; inspect status for the outcome."},
		 {"inputSchema", {{"type", "object"},
						  {"properties", {{"application_id", identifier},
										  {"process_id", {{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}}}}},
						  {"required", Json::array({"application_id", "process_id"})}, {"additionalProperties", false}}}},
		{{"name", "simulator_launch_application"},
		 {"description", "Launch an executable using OpenXR Simulator and return its application ID. "
						 "Use simulator_application_status to observe launch and connection progress."},
		 {"inputSchema", {{"type", "object"},
						  {"properties", {{"executable", text}, {"name", text}, {"working_directory", text},
										  {"runtime_manifest", text},
										  {"arguments", {{"type", "array"}, {"items", text}, {"maxItems", 256}}},
										  {"environment", {{"type", "object"}, {"additionalProperties", text}}}}},
						  {"required", Json::array({"executable"})}, {"additionalProperties", false}}}}
	});
	if (windowless)
		tools.push_back({{"name", "simulator_shutdown"},
			{"description", "Shut down this windowless simulator instance; launched applications keep running."},
			{"inputSchema", {{"type", "object"}, {"properties", Json::object()}, {"additionalProperties", false}}}});
#ifdef SIMXR_COMPANION_AUTOMATION
	const Json integer = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^[1-9][0-9]{0,19}$"}},
		{{"type", "integer"}, {"minimum", 1}, {"maximum", 9007199254740991ULL}}})}};
	const Json seed = {{"anyOf", Json::array({{{"type", "string"}, {"pattern", "^(0|[1-9][0-9]{0,19})$"}},
		{{"type", "integer"}, {"minimum", 0}, {"maximum", 9007199254740991ULL}}})}};
	const auto luauTool = [](const char* name, const char* description, Json properties, Json required)
	{
		return Json{{"name", name}, {"description", description}, {"inputSchema", {{"type", "object"},
			{"properties", std::move(properties)}, {"required", std::move(required)}, {"additionalProperties", false}}}};
	};
	tools.push_back(luauTool("simulator_luau_run", "Run bounded Luau source on one selected application's current XR session. Poll status for completion.",
		{{"application_id", identifier}, {"source", {{"type", "string"}, {"minLength", 1}, {"maxLength", 524288}}},
		 {"name", {{"type", "string"}, {"maxLength", 256}}}, {"seed", seed},
		 {"deadline_seconds", {{"type", "integer"}, {"minimum", 1}, {"maximum", 3600}}},
		 {"memory_mib", {{"type", "integer"}, {"minimum", 1}, {"maximum", 512}}}},
		Json::array({"application_id", "source"})));
	for (const auto& [name, description] : {
		std::pair{"simulator_luau_status", "Read this MCP session's current or selected Luau run, including phase, output and checkpoints."},
		{"simulator_luau_pause", "Request a pause at the next simulator binding boundary; pure Luau code runs until then."},
		{"simulator_luau_resume", "Resume this MCP session's paused Luau run."},
		{"simulator_luau_step", "Advance this MCP session's paused Luau run by one binding boundary."},
		{"simulator_luau_cancel", "Cancel this MCP session's Luau run and release its runtime resources."}})
		tools.push_back(luauTool(name, description, {{"application_id", identifier}, {"run_id", integer}},
			name == std::string_view("simulator_luau_status") ? Json::array({"application_id"})
																		 : Json::array({"application_id", "run_id"})));
#endif
	for (auto& definition : McpDebug::Definitions())
	{
		if (definition.value("name", "") == "debug_export_info")
		{
			auto artifact = definition;
			artifact["name"] = "debug_export_artifact";
			artifact["description"] = "Create an authenticated, short-lived binary download link for a completed image or video export. The export must stay retained.";
			tools.push_back(std::move(artifact));
		}
		tools.push_back(std::move(definition));
	}
	for (auto& definition : MonitorDefinitions())
		tools.push_back(std::move(definition));
	for (auto& definition : McpSequenceJobs::Definitions())
		tools.push_back(std::move(definition));
	return tools;
}

McpServer::McpServer(McpApplication& application, std::uint16_t port, std::string token)
	: m_application(application), m_token(std::move(token))
{
	if (m_token.size() != 64 || !std::ranges::all_of(m_token, [](unsigned char value)
		{ return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f'); }))
		throw std::invalid_argument("SIMXR_MCP_TOKEN must contain 64 lowercase hexadecimal characters");
	m_server.set_payload_max_length(1024 * 1024);
	m_server.set_read_timeout(std::chrono::seconds(5));
	m_server.set_write_timeout(std::chrono::seconds(5));
	m_server.set_keep_alive_max_count(1);
	m_server.set_socket_options(ConfigureMcpListener);
	m_server.new_task_queue = [] { return new httplib::ThreadPool(8, 8, 32); };
	m_server.set_pre_routing_handler([this](const auto& request, auto& response)
	{
		return Authorize(request, response) ? httplib::Server::HandlerResponse::Unhandled
											 : httplib::Server::HandlerResponse::Handled;
	});
	m_server.Post("/mcp", [this](const auto& request, auto& response) { Route(request, response); });
	m_server.Get("/mcp", [](const auto&, auto& response) { response.status = 405; });
	m_server.Get(R"(/artifacts/([0-9a-f]{64}))", [this](const auto& request, auto& response)
		{ DownloadArtifact(request, response); });
	m_server.Delete("/mcp", [this](const auto& request, auto& response) { DeleteSession(request, response); });
	const auto bound = port ? (m_server.bind_to_port("127.0.0.1", port) ? static_cast<int>(port) : -1)
							: m_server.bind_to_any_port("127.0.0.1");
	if (bound <= 0 || bound > 65535)
		throw std::runtime_error("Cannot bind the local MCP endpoint");
	m_port = static_cast<std::uint16_t>(bound);
	try
	{
		m_worker = std::thread([this]
		{
			try { m_server.listen_after_bind(); }
			catch (...) {}
			m_workerExited.store(true);
		});
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(2);
		while (!m_server.is_running() && !m_workerExited.load() && std::chrono::steady_clock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (!m_server.is_running())
			throw std::runtime_error("The local MCP listener failed to start");
		m_reaper = std::thread([this]
		{
			std::unique_lock lock(m_reaperMutex);
			while (!m_reaperChanged.wait_for(lock, std::chrono::seconds(30), [this] { return m_reaperStopping; }))
			{
				lock.unlock();
				try
				{
					ReapExpired();
				}
				catch (const std::exception& error)
				{
					std::fprintf(stderr, "MCP session cleanup failed: %s\n", error.what());
				}
				catch (...)
				{
					std::fputs("MCP session cleanup failed\n", stderr);
				}
				lock.lock();
			}
		});
	}
	catch (...)
	{
		m_server.stop();
		if (m_worker.joinable())
			m_worker.join();
		throw;
	}
}

McpServer::~McpServer()
{
	Stop();
}

std::uint16_t McpServer::Port() const noexcept
{
	return m_port;
}

std::size_t McpServer::ClientSessions() const
{
	std::lock_guard lock(m_sessionMutex);
	return static_cast<std::size_t>(std::ranges::count_if(m_sessions, [](const auto& entry)
		{ return entry.second->initialized; }));
}

void McpServer::Stop() noexcept
{
	m_application.Close();
	{
		std::lock_guard lock(m_reaperMutex);
		m_reaperStopping = true;
	}
	m_reaperChanged.notify_all();
	if (m_reaper.joinable())
		m_reaper.join();
	std::unordered_map<std::string, std::shared_ptr<Session>> sessions;
	{
		std::lock_guard lock(m_sessionMutex);
		m_sessions.swap(sessions);
		m_artifacts.clear();
		for (const auto& [identity, session] : sessions)
		{
			session->automationOwner->active.store(false);
			for (const auto& [id, call] : session->calls)
				call.cancellation->Cancel();
		}
	}
	for (const auto& [identity, session] : sessions)
	{
		session->sequenceJobs.Close();
		session->debug.Close();
	}
	m_server.stop();
	if (m_worker.joinable())
		m_worker.join();
}

void McpServer::ReapExpired()
{
	std::vector<std::shared_ptr<Session>> expired;
	{
		std::lock_guard lock(m_sessionMutex);
		const auto now = std::chrono::steady_clock::now();
		for (auto entry = m_sessions.begin(); entry != m_sessions.end();)
		{
			if (entry->second->calls.empty() && now - entry->second->lastUsed > std::chrono::minutes(30))
			{
				entry->second->automationOwner->active.store(false);
				expired.push_back(entry->second);
				entry = m_sessions.erase(entry);
			}
			else
				++entry;
		}
		std::erase_if(m_artifacts, [&](const auto& item)
			{ return item.second->expires <= now || !m_sessions.contains(item.second->session); });
	}
	for (const auto& session : expired)
	{
		session->sequenceJobs.Close();
		session->debug.Close();
	}
}

bool McpServer::Authorize(const httplib::Request& request, httplib::Response& response) const
{
	const auto host = request.get_header_value("Host");
	const auto local = "127.0.0.1:" + std::to_string(m_port);
	const auto localhost = "localhost:" + std::to_string(m_port);
	if (request.get_header_value_count("Host") != 1 || (host != local && host != localhost) ||
		request.get_header_value_count("Origin") > 1 ||
		(request.has_header("Origin") && request.get_header_value("Origin") != "http://" + local &&
		 request.get_header_value("Origin") != "http://" + localhost))
	{
		response.status = 403;
		return false;
	}
	const auto authorization = request.get_header_value("Authorization");
	if (request.get_header_value_count("Authorization") != 1 || !authorization.starts_with("Bearer ") ||
		!EqualToken(std::string_view(authorization).substr(7), m_token))
	{
		response.status = 401;
		response.set_header("WWW-Authenticate", "Bearer");
		return false;
	}
	const auto version = request.get_header_value("MCP-Protocol-Version");
	if (request.get_header_value_count("MCP-Protocol-Version") > 1 ||
		(!version.empty() && !SupportedVersion(version)))
	{
		response.status = 400;
		return false;
	}
	return true;
}

void McpServer::DeleteSession(const httplib::Request& request, httplib::Response& response)
{
	if (request.get_header_value_count("MCP-Session-Id") != 1)
	{
		response.status = 400;
		return;
	}
	std::shared_ptr<Session> session;
	{
		std::lock_guard lock(m_sessionMutex);
		const auto found = m_sessions.find(request.get_header_value("MCP-Session-Id"));
		if (found == m_sessions.end())
		{
			response.status = 404;
			return;
		}
		session = found->second;
		const auto version = request.get_header_value("MCP-Protocol-Version");
		if (!version.empty() && version != session->version)
		{
			response.status = 400;
			return;
		}
		session->automationOwner->active.store(false);
		m_sessions.erase(found);
		std::erase_if(m_artifacts, [&](const auto& item) { return item.second->session == request.get_header_value("MCP-Session-Id"); });
		for (const auto& [id, call] : session->calls)
			call.cancellation->Cancel();
	}
	session->sequenceJobs.Close();
	session->debug.Close();
	response.status = 204;
}

void McpServer::DownloadArtifact(const httplib::Request& request, httplib::Response& response)
{
	if (request.get_header_value_count("MCP-Session-Id") > 1)
	{
		response.status = 400;
		return;
	}
	std::shared_ptr<Artifact> artifact;
	std::shared_ptr<Session> session;
	{
		std::lock_guard lock(m_sessionMutex);
		const auto found = m_artifacts.find(request.matches[1].str());
		if (found == m_artifacts.end() || found->second->expires <= std::chrono::steady_clock::now())
		{
			response.status = 404;
			return;
		}
		artifact = found->second;
		const auto owner = m_sessions.find(artifact->session);
		if (owner == m_sessions.end() || !owner->second->initialized ||
			(request.has_header("MCP-Session-Id") && request.get_header_value("MCP-Session-Id") != artifact->session))
		{
			response.status = 404;
			return;
		}
		session = owner->second;
		const auto version = request.get_header_value("MCP-Protocol-Version");
		if (!version.empty() && version != session->version)
		{
			response.status = 400;
			return;
		}
		session->lastUsed = std::chrono::steady_clock::now();
	}
	auto active = m_activeDownloads.load();
	for (;;)
	{
		if (active >= 2)
		{
			response.status = 503;
			return;
		}
		if (m_activeDownloads.compare_exchange_weak(active, active + 1))
			break;
	}
	std::shared_ptr<McpDownloadSlot> slot;
	try
	{
		slot = std::make_shared<McpDownloadSlot>(m_activeDownloads);
	}
	catch (...)
	{
		--m_activeDownloads;
		throw;
	}
	try
	{
		const auto cancellation = std::make_shared<McpCancellation>();
		const auto result = session->debug.Call(m_application, "debug_export_info",
			{{"connection", artifact->connection}, {"export", std::to_string(artifact->ticket)}}, cancellation);
		if (!result || result->value("isError", true))
		{
			response.status = 404;
			return;
		}
		const auto info = Json::parse(result->at("content").at(0).at("text").get<std::string>());
		if (info.value("phase", "") != "complete" || info.value("total_bytes", "") != std::to_string(artifact->size))
		{
			response.status = 404;
			return;
		}
	}
	catch (...)
	{
		response.status = 404;
		return;
	}
	response.set_header("Cache-Control", "no-store");
	response.set_header("X-Content-Type-Options", "nosniff");
	response.set_header("Content-Disposition", artifact->mediaType == "video/mp4"
		? "attachment; filename=simxr-clip.mp4" : "attachment; filename=simxr-image.raw");
	response.set_content_provider(static_cast<std::size_t>(artifact->size), artifact->mediaType,
		[this, session, artifact, slot](std::size_t offset, std::size_t length, httplib::DataSink& sink)
		{
			(void)slot;
			if (!length || offset >= artifact->size)
				return false;
			try
			{
				const auto amount = std::min<std::uint64_t>({length, artifact->size - offset, control::MaxPayload - 4096});
				const auto bytes = session->debug.ReadArtifact(m_application, artifact->connection, artifact->ticket, offset, amount);
				return sink.write(reinterpret_cast<const char*>(bytes.data()), bytes.size());
			}
			catch (...)
			{
				return false;
			}
		});
}

void McpServer::Route(const httplib::Request& request, httplib::Response& response)
{
	const auto version = request.get_header_value("MCP-Protocol-Version");
	const auto key = request.get_header_value("X-Simxr-Cancellation-Key");
	if (request.get_header_value_count("X-Simxr-Cancellation-Key") > 1 ||
		(!key.empty() && !ValidCancellationKey(key)))
	{
		response.status = 400;
		return;
	}
	const auto contentType = request.get_header_value("Content-Type");
	if (request.get_header_value_count("Content-Type") != 1 ||
		(contentType != "application/json" && !contentType.starts_with("application/json;")))
	{
		response.status = 415;
		return;
	}
	const auto document = Json::parse(request.body, nullptr, false);
	if (document.is_discarded())
	{
		response.status = 400;
		response.set_content(RpcError(nullptr, -32700, "Parse error").dump(), "application/json");
		return;
	}
	if (!document.is_object() || !document.contains("jsonrpc") || !document["jsonrpc"].is_string() ||
		document["jsonrpc"] != "2.0" ||
		!document.contains("method") || !document["method"].is_string())
	{
		response.status = 400;
		response.set_content(RpcError(nullptr, -32600, "Invalid request").dump(), "application/json");
		return;
	}
	const bool notification = !document.contains("id");
	if (!notification && !document["id"].is_string() && !document["id"].is_number_integer())
	{
		response.status = 400;
		response.set_content(RpcError(nullptr, -32600, "Invalid request ID").dump(), "application/json");
		return;
	}
	const auto method = document["method"].get<std::string>();
	if (method == "initialize")
	{
		if (notification || request.get_header_value_count("MCP-Session-Id") != 0)
		{
			response.status = 400;
			return;
		}
		const auto& id = document["id"];
		const auto params = document.value("params", Json::object());
		if (!params.is_object() || !params.contains("protocolVersion") || !params["protocolVersion"].is_string() ||
			!params.contains("capabilities") || !params["capabilities"].is_object() ||
			!params.contains("clientInfo") || !params["clientInfo"].is_object() ||
			!params["clientInfo"].contains("name") || !params["clientInfo"]["name"].is_string() ||
			!params["clientInfo"].contains("version") || !params["clientInfo"]["version"].is_string())
		{
			response.set_content(RpcError(id, -32602, "Invalid initialization parameters").dump(), "application/json");
			return;
		}
		const auto requested = params["protocolVersion"].get<std::string>();
		auto session = std::make_shared<Session>();
		session->version = SupportedVersion(requested) ? requested : "2025-11-25";
		const auto sessionId = NewMcpIdentifier();
		ReapExpired();
		{
			std::lock_guard lock(m_sessionMutex);
			if (m_sessions.size() >= 128 || !m_sessions.emplace(sessionId, session).second)
			{
				response.status = 503;
				return;
			}
		}
		response.set_header("MCP-Session-Id", sessionId);
		Json result = {{"protocolVersion", session->version},
				  {"capabilities", {{"tools", {{"listChanged", false}}}}},
				  {"serverInfo", {{"name", "openxr-simulator"}, {"version", simxr::Version}}}};
		response.set_content(Json({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}).dump(),
							 "application/json");
		return;
	}

	if (request.get_header_value_count("MCP-Session-Id") != 1)
	{
		response.status = 400;
		return;
	}
	std::shared_ptr<Session> session;
	std::shared_ptr<McpCancellation> cancellation;
	std::string callId;
	{
		std::lock_guard lock(m_sessionMutex);
		const auto found = m_sessions.find(request.get_header_value("MCP-Session-Id"));
		if (found == m_sessions.end())
		{
			response.status = 404;
			return;
		}
		session = found->second;
		const auto now = std::chrono::steady_clock::now();
		if (session->calls.empty() && now - session->lastUsed > std::chrono::minutes(30))
		{
			session->automationOwner->active.store(false);
			m_sessions.erase(found);
			response.status = 404;
			return;
		}
		if (!version.empty() && version != session->version)
		{
			response.status = 400;
			return;
		}
		session->lastUsed = now;
		if (notification && method == "notifications/initialized")
			session->initialized = true;
		else if (notification && method == "notifications/cancelled")
		{
			const auto params = document.value("params", Json::object());
			if (params.is_object() && params.contains("requestId") &&
				(params["requestId"].is_string() || params["requestId"].is_number_integer()))
			{
				const auto call = session->calls.find(params["requestId"].dump());
				if (call != session->calls.end())
				{
					if (key.empty() || key == call->second.key)
						call->second.cancellation->Cancel();
				}
				else if (!key.empty())
				{
					if (session->earlyCancellations.size() >= 64 && !session->earlyCancellations.contains(key))
					{
						for (const auto& [id, active] : session->calls)
							active.cancellation->Cancel();
						session->automationOwner->active.store(false);
						m_sessions.erase(found);
						response.status = 503;
						return;
					}
					session->earlyCancellations.insert(key);
				}
			}
		}
		if (!notification && method != "ping" && !session->initialized)
		{
			response.set_content(RpcError(document["id"], -32600, "Session is not initialized").dump(),
							 "application/json");
			return;
		}
		if (!notification && method == "tools/call")
		{
			if (!key.empty() && session->earlyCancellations.erase(key))
			{
				response.status = 202;
				return;
			}
			callId = document["id"].dump();
			if (session->calls.contains(callId))
			{
				response.set_content(RpcError(document["id"], -32600, "Request ID is already active").dump(),
								 "application/json");
				return;
			}
			if (m_activeCalls >= 4)
			{
				response.status = 503;
				return;
			}
			cancellation = std::make_shared<McpCancellation>();
			session->calls.emplace(callId, Session::Call{cancellation, key});
			++m_activeCalls;
		}
	}
	if (notification)
	{
		response.status = 202;
		return;
	}
	const auto id = document["id"];
	Json result;
	if (method == "ping")
		result = Json::object();
	else if (method == "tools/list")
		result = {{"tools", Tools(m_application.Windowless())}};
	else if (method == "tools/call")
	{
		const auto params = document.value("params", Json::object());
		const auto release = [&]
		{
			std::lock_guard lock(m_sessionMutex);
			session->calls.erase(callId);
			--m_activeCalls;
		};
		if (!params.is_object() || !params.contains("name") || !params["name"].is_string() ||
			(params.contains("arguments") && !params["arguments"].is_object()))
		{
			release();
			response.set_content(RpcError(id, -32602, "Invalid tool call parameters").dump(), "application/json");
			return;
		}
		std::optional<Json> callResult;
		try
		{
			const auto name = params["name"].get<std::string>();
			const auto arguments = params.value("arguments", Json::object());
			if (name == "debug_export_artifact")
			{
				std::uint64_t artifactRevision;
				{
					std::lock_guard lock(m_sessionMutex);
					artifactRevision = session->artifactRevision;
				}
				callResult = session->debug.Call(m_application, "debug_export_info", arguments, cancellation);
				if (callResult && !callResult->value("isError", true) && !cancellation->Requested())
				{
					const auto fail = [](const char* message)
					{
						return Json{{"content", Json::array({{{"type", "text"},
							{"text", Json({{"ok", false}, {"error", message}}).dump()}}})}, {"isError", true}};
					};
					const auto info = Json::parse(callResult->at("content").at(0).at("text").get<std::string>());
					const auto number = [](const Json& value, const char* name)
					{
						const auto text = value.at(name).get<std::string>();
						std::uint64_t result = 0;
						const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
						if (text.empty() || parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
							throw std::runtime_error("Invalid export metadata");
						return result;
					};
					const auto size = number(info, "total_bytes");
					const auto ticket = number(info, "export");
					if (info.value("phase", "") != "complete" || !size || size > 512ULL * 1024 * 1024 || !ticket)
						callResult = fail("Export is incomplete or exceeds the artifact size limit");
					else
					{
						const auto identity = NewMcpIdentifier();
						const auto owner = request.get_header_value("MCP-Session-Id");
						const auto connection = arguments.at("connection").get<std::string>();
						const auto media = info.value("media_type", "") == "video/mp4" ? "video/mp4" : "application/octet-stream";
						std::string selected;
						{
							std::lock_guard lock(m_sessionMutex);
							const auto now = std::chrono::steady_clock::now();
							std::erase_if(m_artifacts, [&](const auto& item)
								{ return item.second->expires <= now || !m_sessions.contains(item.second->session); });
							if (const auto active = m_sessions.find(owner); active != m_sessions.end() && active->second == session &&
								session->artifactRevision == artifactRevision)
							{
								std::size_t count = 0;
								for (const auto& [id, existing] : m_artifacts)
								{
									if (existing->session != owner)
										continue;
									++count;
									if (existing->connection == connection && existing->ticket == ticket && existing->size == size)
									{
										existing->expires = now + std::chrono::minutes(5);
										selected = id;
									}
								}
								if (selected.empty() && count < 8 && m_artifacts.size() < 128)
								{
									auto artifact = std::make_shared<Artifact>();
									artifact->session = owner;
									artifact->connection = connection;
									artifact->ticket = ticket;
									artifact->size = size;
									artifact->mediaType = media;
									artifact->expires = now + std::chrono::minutes(5);
									if (m_artifacts.emplace(identity, std::move(artifact)).second)
										selected = identity;
								}
							}
						}
						if (selected.empty())
							callResult = fail("Artifact unavailable: the connection changed, the session closed, or capacity was reached");
						else
						{
							const auto uri = "http://127.0.0.1:" + std::to_string(m_port) + "/artifacts/" + selected;
							Json metadata = info;
							metadata["expires_in_seconds"] = 300;
							metadata["download_auth"] = "Bearer token required; keep the export and MCP session open";
							callResult = Json{{"content", Json::array({{{"type", "text"}, {"text", metadata.dump()}},
								{{"type", "resource_link"}, {"uri", uri}, {"name", media == std::string_view("video/mp4") ? "simxr-clip.mp4" : "simxr-image.raw"},
								 {"mimeType", media}, {"size", size}}})}, {"isError", false}};
						}
					}
				}
				else if (cancellation->Requested())
					callResult.reset();
			}
			else
			{
				callResult = McpSequenceJobs::Handles(name) ? session->sequenceJobs.Call(m_application, session->debug, name, arguments, cancellation) :
					HandlesMonitor(name) ? m_application.Call(name, arguments, cancellation) :
					McpDebug::Handles(name) ? session->debug.Call(m_application, name, arguments, cancellation) :
					m_application.Call(name, arguments, cancellation, session->automationOwner);
				if ((name == "debug_release_export" || name == "debug_disconnect") && callResult &&
					!callResult->value("isError", true))
				{
					const auto connection = arguments.at("connection").get<std::string>();
					std::uint64_t ticket = 0;
					if (name == "debug_release_export")
					{
						const auto& value = arguments.at("export");
						if (value.is_string())
						{
							const auto text = value.get<std::string>();
							const auto parsed = std::from_chars(text.data(), text.data() + text.size(), ticket);
							if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size())
								throw std::runtime_error("Invalid released export identity");
						}
						else
							ticket = value.get<std::uint64_t>();
					}
					const auto owner = request.get_header_value("MCP-Session-Id");
					std::lock_guard lock(m_sessionMutex);
					++session->artifactRevision;
					std::erase_if(m_artifacts, [&](const auto& item)
					{
						return item.second->session == owner && item.second->connection == connection &&
							(name == "debug_disconnect" || item.second->ticket == ticket);
					});
				}
			}
		}
		catch (...)
		{
			release();
			throw;
		}
		release();
		if (!callResult)
		{
			response.status = 202;
			return;
		}
		result = std::move(*callResult);
	}
	else
	{
		response.set_content(RpcError(id, -32601, "Method not found").dump(), "application/json");
		return;
	}
	response.set_content(Json({{"jsonrpc", "2.0"}, {"id", id}, {"result", std::move(result)}}).dump(),
						 "application/json");
}
} // namespace simxr::app
