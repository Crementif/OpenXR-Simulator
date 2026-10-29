#include "app/mcp/McpPch.h"
#include "app/mcp/McpConnector.h"
#include "app/mcp/McpCredentials.h"
#ifdef _WIN32
#include "app/mcp/windows/McpCancelablePost.h"
#endif

#include <iostream>

#include <httplib.h>

namespace simxr::app
{
using Json = nlohmann::json;
using namespace std::chrono_literals;

static bool SupportedVersion(std::string_view version)
{
	return version == "2025-03-26" || version == "2025-06-18" || version == "2025-11-25";
}

static httplib::Headers Headers(const std::string& token, const std::string& session = {},
								const std::string& version = {})
{
	httplib::Headers headers{{"Authorization", "Bearer " + token}, {"Accept", "application/json, text/event-stream"}};
	if (!session.empty())
		headers.emplace("MCP-Session-Id", session);
	if (!version.empty())
		headers.emplace("MCP-Protocol-Version", version);
	return headers;
}

static httplib::Result Post(httplib::Client& client, const httplib::Headers& headers, const Json& message,
							std::chrono::seconds readTimeout)
{
	client.set_connection_timeout(1s);
	client.set_read_timeout(readTimeout);
	client.set_write_timeout(2s);
	client.set_keep_alive(false);
	client.set_payload_max_length(16 * 1024 * 1024);
	return client.Post("/mcp", headers, message.dump(), "application/json");
}

static httplib::Result Post(std::uint16_t port, const httplib::Headers& headers, const Json& message)
{
	httplib::Client client("127.0.0.1", port);
	return Post(client, headers, message, 7s);
}

static void DeleteSession(std::uint16_t port, const std::string& token, const std::string& session,
						  const std::string& version) noexcept
{
	try
	{
		httplib::Client client("127.0.0.1", port);
		client.set_connection_timeout(1s);
		client.set_read_timeout(2s);
		client.Delete("/mcp", Headers(token, session, version));
	}
	catch (...)
	{}
}

static Json Error(const Json& id, int code, const std::string& message)
{
	return {{"jsonrpc", "2.0"}, {"id", id}, {"error", {{"code", code}, {"message", message}}}};
}

static Json ListChanged()
{
	return {{"jsonrpc", "2.0"}, {"method", "notifications/tools/list_changed"}};
}

McpConnector::McpConnector(std::uint16_t port, std::string token) : m_fixedEndpoint(McpEndpoint{port, std::move(token)})
{
	if (m_fixedEndpoint->token.size() != 64 || !std::ranges::all_of(m_fixedEndpoint->token, [](unsigned char value) {
			return (value >= '0' && value <= '9') || (value >= 'a' && value <= 'f');
		}))
		throw std::invalid_argument("SIMXR_MCP_TOKEN must contain 64 lowercase hexadecimal characters");
}

#ifdef SIMXR_MCP_DISCOVERY
McpConnector::McpConnector(std::unique_ptr<McpInstanceDiscovery> discovery) : m_discovery(std::move(discovery))
{
	if (!m_discovery)
		throw std::invalid_argument("MCP discovery is required");
}
#endif

McpConnector::~McpConnector()
{
	Stop();
}

void McpConnector::Send(const Json& message)
{
	std::lock_guard lock(m_outputMutex);
	if (!m_closed.load())
		std::cout << message.dump() << '\n' << std::flush;
}

void McpConnector::Invalidate(std::uint64_t generation)
{
	std::optional<Connection> retired;
	{
		std::lock_guard lock(m_stateMutex);
		if (m_connection && m_connection->generation == generation)
		{
			retired = std::move(m_connection);
			m_connection.reset();
			m_stateChanged.notify_all();
		}
	}
	if (retired)
		DeleteSession(retired->endpoint.port, retired->endpoint.token, retired->session, retired->version);
}

void McpConnector::Monitor()
{
	for (;;)
	{
		std::optional<Connection> current;
		Json initializationParams;
		{
			std::unique_lock lock(m_stateMutex);
			m_stateChanged.wait(lock, [&] { return m_stopping || !m_requestedVersion.empty(); });
			if (m_stopping)
				return;
			current = m_connection;
			initializationParams = m_initializationParams;
		}
		try
		{
			if (!current)
			{
				auto endpoint = m_fixedEndpoint;
#ifdef SIMXR_MCP_DISCOVERY
				if (m_discovery)
				{
					endpoint = m_discovery->Read();
					std::lock_guard lock(m_stateMutex);
					m_unavailableReason = m_discovery->StartupError();
					m_stateChanged.notify_all();
				}
#endif
				if (!endpoint)
				{
					std::unique_lock lock(m_stateMutex);
					m_stateChanged.wait_for(lock, 1s, [&] { return m_stopping; });
					if (m_stopping)
						return;
					continue;
				}
				const Json initialize = {{"jsonrpc", "2.0"},
										 {"id", "connector-initialize"},
										 {"method", "initialize"},
										 {"params", initializationParams}};
				const auto response = Post(endpoint->port, Headers(endpoint->token), initialize);
				if (response && response->status == 200)
				{
					const auto body = Json::parse(response->body, nullptr, false);
					const auto session = response->get_header_value("MCP-Session-Id");
					if (!body.is_discarded() && body.is_object() &&
						body.value("id", Json()) == "connector-initialize" && body.contains("result") &&
						body["result"].is_object() && body["result"].contains("protocolVersion") &&
						body["result"]["protocolVersion"].is_string() &&
						SupportedVersion(body["result"]["protocolVersion"].get<std::string>()) && !session.empty())
					{
						const auto version = body["result"]["protocolVersion"].get<std::string>();
						const auto initialized = Post(endpoint->port, Headers(endpoint->token, session, version),
													  {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}});
						if (initialized && initialized->status == 202)
						{
							bool notify = false, stopping = false;
							{
								std::lock_guard lock(m_stateMutex);
								stopping = m_stopping;
								if (!stopping)
								{
									m_connection = Connection{*endpoint, session, version, body, ++m_generation};
									notify = m_clientInitialized && m_generation > m_reportedGeneration;
									if (notify)
										m_reportedGeneration = m_generation;
									m_stateChanged.notify_all();
								}
							}
							if (stopping)
							{
								DeleteSession(endpoint->port, endpoint->token, session, version);
								return;
							}
							if (notify)
								Send(ListChanged());
						}
						else
							DeleteSession(endpoint->port, endpoint->token, session, version);
					}
				}
			}
			else
			{
#ifdef SIMXR_MCP_DISCOVERY
				if (m_discovery)
				{
					const auto endpoint = m_discovery->Read();
					if (!endpoint || endpoint->port != current->endpoint.port ||
						endpoint->token != current->endpoint.token)
					{
						Invalidate(current->generation);
						continue;
					}
				}
#endif
				const auto response =
					Post(current->endpoint.port, Headers(current->endpoint.token, current->session, current->version),
						 {{"jsonrpc", "2.0"}, {"id", "connector-ping"}, {"method", "ping"}});
				if (!response || response->status != 200)
					Invalidate(current->generation);
			}
		}
		catch (const std::exception&)
		{
			if (current)
				Invalidate(current->generation);
		}
		std::unique_lock lock(m_stateMutex);
		m_stateChanged.wait_for(lock, 1s, [&] { return m_stopping || (current && !m_connection); });
		if (m_stopping)
			return;
	}
}

void McpConnector::Handle(const std::shared_ptr<Job>& job)
{
	const auto& message = job->message;
	const bool notification = !message.contains("id");
	const auto method = message["method"].get<std::string>();
	const Json id = notification ? Json() : message["id"];
	if (method == "initialize")
	{
		const auto params = message.value("params", Json::object());
		if (notification || !params.is_object() || !params.contains("protocolVersion") ||
			!params["protocolVersion"].is_string() || !params.contains("capabilities") ||
			!params["capabilities"].is_object() || !params.contains("clientInfo") ||
			!params["clientInfo"].is_object() || !params["clientInfo"].contains("name") ||
			!params["clientInfo"]["name"].is_string() || !params["clientInfo"].contains("version") ||
			!params["clientInfo"]["version"].is_string())
		{
			if (!notification)
				Send(Error(id, -32602, "Invalid initialization parameters"));
			return;
		}
		std::unique_lock lock(m_stateMutex);
		if (m_handshakeActive || m_handshakeComplete ||
			(!m_requestedVersion.empty() && params != m_initializationParams))
		{
			lock.unlock();
			Send(Error(id, -32600, "The connector is already initializing or initialized"));
			return;
		}
		m_handshakeActive = true;
		m_initializationParams = params;
		m_requestedVersion = params["protocolVersion"].get<std::string>();
		if (m_requestedVersion.empty())
			m_requestedVersion = "2025-11-25";
		m_stateChanged.notify_all();
		if (!m_stateChanged.wait_for(
				lock, 10s, [&] { return m_stopping || m_connection.has_value() || !m_unavailableReason.empty(); }) ||
			!m_connection)
		{
			m_handshakeActive = false;
			const auto reason = m_unavailableReason.empty() ? "OpenXR Simulator is unavailable" : m_unavailableReason;
			lock.unlock();
			Send(Error(id, -32000, reason));
			return;
		}
		auto response = m_connection->initialization;
		m_reportedGeneration = m_connection->generation;
		m_handshakeActive = false;
		m_handshakeComplete = true;
		lock.unlock();
		response["id"] = id;
		if (response["result"].contains("capabilities") && response["result"]["capabilities"].is_object() &&
			response["result"]["capabilities"].contains("tools") &&
			response["result"]["capabilities"]["tools"].is_object())
			response["result"]["capabilities"]["tools"]["listChanged"] = true;
		Send(response);
		return;
	}
	if (notification && method == "notifications/initialized")
	{
		bool notify = false;
		{
			std::lock_guard lock(m_stateMutex);
			m_clientInitialized = true;
			notify = m_connection && m_connection->generation > m_reportedGeneration;
			if (notify)
				m_reportedGeneration = m_connection->generation;
		}
		if (notify)
			Send(ListChanged());
		return;
	}
	std::optional<Connection> current;
	std::string unavailableReason;
	{
		std::lock_guard lock(m_stateMutex);
		current = m_connection;
		unavailableReason = m_unavailableReason;
	}
	if (!current)
	{
		if (!notification)
			Send(Error(id, -32000,
					   unavailableReason.empty()
						   ? "OpenXR Simulator is unavailable; the connector will reconnect when it returns"
						   : unavailableReason));
		return;
	}
	{
		std::lock_guard lock(m_queueMutex);
		if (job->cancelled)
			return;
	}
	auto headers = Headers(current->endpoint.token, current->session, current->version);
	if (!job->cancellationKey.empty())
		headers.emplace("X-Simxr-Cancellation-Key", job->cancellationKey);
	httplib::Result response;
	if (job->callId.empty())
		response = Post(current->endpoint.port, headers, message);
	else
	{
		const bool videoSave = message.contains("params") && message["params"].is_object() &&
							   message["params"].contains("name") && message["params"]["name"].is_string() &&
							   message["params"]["name"] == "debug_save_video";
		const auto readTimeout = videoSave ? 605s : 35s;
#ifdef _WIN32
		response = windows::CancelablePost(current->endpoint.port, headers, message.dump(), m_closed, readTimeout);
#else
		auto transport = std::make_shared<httplib::Client>("127.0.0.1", current->endpoint.port);
		{
			std::lock_guard lock(m_queueMutex);
			if (m_closed.load())
				return;
			job->transport = transport;
		}
		const auto release = [&] {
			std::lock_guard lock(m_queueMutex);
			job->transport.reset();
			m_queueChanged.notify_all();
		};
		try
		{
			response = Post(*transport, headers, message, readTimeout);
		}
		catch (...)
		{
			release();
			throw;
		}
		release();
#endif
	}
	if (notification)
	{
		if (!response || response->status != 202)
			Invalidate(current->generation);
		return;
	}
	if (response && response->status == 202)
	{
		std::lock_guard lock(m_queueMutex);
		if (job->cancelled)
			return;
	}
	if (response && response->status == 200)
	{
		const auto body = Json::parse(response->body, nullptr, false);
		if (!body.is_discarded() && body.is_object() && body.value("id", Json()) == id &&
			(body.contains("result") || body.contains("error")))
		{
			Send(body);
			return;
		}
	}
	if (!response || response->status != 503)
		Invalidate(current->generation);
	Send(Error(
		id, -32000,
		method == "tools/call"
			? "The simulator request was interrupted; its outcome may be unknown. Inspect applications before retrying."
			: "The simulator request was interrupted; the connector will reconnect when it returns"));
}

void McpConnector::Work(bool notifications)
{
	for (;;)
	{
		std::shared_ptr<Job> job;
		{
			std::unique_lock lock(m_queueMutex);
			auto& queue = notifications ? m_notifications : m_jobs;
			m_queueChanged.wait(lock, [&] { return m_closed.load() || !queue.empty(); });
			if (m_closed.load())
				return;
			job = std::move(queue.front());
			queue.pop_front();
			m_queueChanged.notify_all();
			if (job->cancelled)
			{
				m_calls.erase(job->callId);
				continue;
			}
			job->started = true;
		}
		const auto id = job->message.value("id", Json());
		const bool hasId = job->message.contains("id");
		try
		{
			Handle(job);
		}
		catch (const std::exception& error)
		{
			std::fprintf(stderr, "MCP connector request: %s\n", error.what());
			if (hasId)
				Send(Error(id, -32603, "Connector request failed"));
		}
		if (!job->callId.empty())
		{
			std::lock_guard lock(m_queueMutex);
			m_calls.erase(job->callId);
		}
	}
}

void McpConnector::Run()
{
	try
	{
		m_monitor = std::thread([this] { Monitor(); });
		for (unsigned index = 0; index < 8; ++index)
			m_workers.emplace_back([this] { Work(false); });
		m_workers.emplace_back([this] { Work(true); });
		std::string line;
		bool oversized = false;
		for (char byte; std::cin.get(byte);)
		{
			if (byte != '\n')
			{
				if (line.size() < 1024 * 1024)
					line.push_back(byte);
				else
					oversized = true;
				continue;
			}
			auto message = oversized ? Json(Json::value_t::discarded) : Json::parse(line, nullptr, false);
			line.clear();
			oversized = false;
			if (message.is_discarded())
			{
				Send(Error(nullptr, -32700, "Invalid or oversized JSON message"));
				continue;
			}
			if (!message.is_object() || message.value("jsonrpc", Json()) != "2.0" || !message.contains("method") ||
				!message["method"].is_string() ||
				(message.contains("id") && !message["id"].is_string() && !message["id"].is_number_integer()))
			{
				Send(Error(nullptr, -32600, "Invalid request"));
				continue;
			}
			const bool notification = !message.contains("id");
			const bool cancel = notification && message["method"] == "notifications/cancelled";
			const bool call = !notification && message["method"] == "tools/call";
			std::string cancellationKey = call ? NewMcpIdentifier() : std::string{};
			bool queued = false, duplicate = false, cancelledBeforeDispatch = false;
			std::shared_ptr<Job> immediate;
			{
				std::lock_guard lock(m_queueMutex);
				if (cancel && message.contains("params") && message["params"].is_object() &&
					message["params"].contains("requestId") &&
					(message["params"]["requestId"].is_string() || message["params"]["requestId"].is_number_integer()))
				{
					const auto found = m_calls.find(message["params"]["requestId"].dump());
					if (found != m_calls.end())
					{
						found->second->cancelled = true;
						cancelledBeforeDispatch = !found->second->started;
						cancellationKey = found->second->cancellationKey;
					}
				}
				const auto callId = call ? message["id"].dump() : std::string{};
				duplicate = call && m_calls.contains(callId);
				auto& queue = notification ? m_notifications : m_jobs;
				if (!duplicate && !cancelledBeforeDispatch && (notification || queue.size() < 32))
				{
					auto job = std::make_shared<Job>();
					job->message = message;
					job->callId = callId;
					job->cancellationKey = std::move(cancellationKey);
					if (call)
						m_calls.emplace(callId, job);
					if (notification && queue.size() >= 32)
						immediate = std::move(job);
					else if (cancel)
						queue.push_front(job);
					else
						queue.push_back(job);
					queued = !immediate;
				}
			}
			if (queued)
				m_queueChanged.notify_all();
			else if (immediate)
			{
				try
				{
					Handle(immediate);
				}
				catch (const std::exception& error)
				{
					std::fprintf(stderr, "MCP connector notification: %s\n", error.what());
				}
			}
			else if (duplicate)
				Send(Error(message["id"], -32600, "Request ID is already active"));
			else if (!notification)
				Send(Error(message["id"], -32000, "The connector request queue is full"));
		}
	}
	catch (...)
	{
		Stop();
		throw;
	}
	Stop();
}

void McpConnector::Stop() noexcept
{
	if (m_closed.exchange(true))
		return;
	{
		std::lock_guard lock(m_stateMutex);
		m_stopping = true;
	}
	m_stateChanged.notify_all();
	m_queueChanged.notify_all();
	if (m_monitor.joinable())
		m_monitor.join();
	std::optional<Connection> current;
	{
		std::lock_guard lock(m_stateMutex);
		current = std::move(m_connection);
		m_connection.reset();
	}
	if (current)
		DeleteSession(current->endpoint.port, current->endpoint.token, current->session, current->version);
	for (;;)
	{
		std::vector<std::shared_ptr<httplib::Client>> active;
		{
			std::lock_guard lock(m_queueMutex);
			for (const auto& [identity, job] : m_calls)
				if (job->transport)
					active.push_back(job->transport);
		}
		if (active.empty())
			break;
		for (const auto& transport : active)
			transport->stop();
		std::unique_lock lock(m_queueMutex);
		m_queueChanged.wait_for(lock, 10ms);
	}
	for (auto& worker : m_workers)
		if (worker.joinable())
			worker.join();
}
} // namespace simxr::app
