#pragma once

#include <atomic>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>
#include "app/mcp/McpInstance.h"

namespace httplib
{
class Client;
}

namespace simxr::app
{
class McpConnector
{
  public:
	McpConnector(std::uint16_t port, std::string token);
#ifdef SIMXR_MCP_DISCOVERY
	explicit McpConnector(std::unique_ptr<McpInstanceDiscovery> discovery);
#endif
	~McpConnector();
	McpConnector(const McpConnector&) = delete;
	McpConnector& operator=(const McpConnector&) = delete;
	void Run();

  private:
	struct Connection
	{
		McpEndpoint endpoint;
		std::string session, version;
		nlohmann::json initialization;
		std::uint64_t generation = 0;
	};
	struct Job
	{
		nlohmann::json message;
		std::string callId, cancellationKey;
		bool started = false, cancelled = false;
		std::shared_ptr<httplib::Client> transport;
	};
	void Monitor();
	void Work(bool notifications);
	void Handle(const std::shared_ptr<Job>& job);
	void Send(const nlohmann::json& message);
	void Invalidate(std::uint64_t generation);
	void Stop() noexcept;
	std::optional<McpEndpoint> m_fixedEndpoint;
#ifdef SIMXR_MCP_DISCOVERY
	std::unique_ptr<McpInstanceDiscovery> m_discovery;
#endif
	std::mutex m_stateMutex, m_outputMutex, m_queueMutex;
	std::condition_variable m_stateChanged, m_queueChanged;
	std::optional<Connection> m_connection;
	nlohmann::json m_initializationParams;
	std::string m_requestedVersion;
	std::string m_unavailableReason;
	std::uint64_t m_generation = 0, m_reportedGeneration = 0;
	bool m_handshakeActive = false, m_handshakeComplete = false, m_clientInitialized = false, m_stopping = false;
	std::atomic<bool> m_closed{false};
	std::deque<std::shared_ptr<Job>> m_jobs;
	std::deque<std::shared_ptr<Job>> m_notifications;
	std::unordered_map<std::string, std::shared_ptr<Job>> m_calls;
	std::thread m_monitor;
	std::vector<std::thread> m_workers;
};
} // namespace simxr::app
