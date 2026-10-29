#pragma once

#include "app/mcp/McpApplication.h"

#include <atomic>
#include <cstdint>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <unordered_map>

#include <httplib.h>

namespace simxr::app
{
// An explicitly enabled loopback endpoint. Credentials never enter a launch profile.
class McpServer
{
public:
	McpServer(McpApplication& application, std::uint16_t port, std::string token);
	~McpServer();
	McpServer(const McpServer&) = delete;
	McpServer& operator=(const McpServer&) = delete;

	void Stop() noexcept;

	std::uint16_t Port() const noexcept;
	std::size_t ClientSessions() const;

private:
	void Route(const httplib::Request& request, httplib::Response& response);
	void DownloadArtifact(const httplib::Request& request, httplib::Response& response);
	void DeleteSession(const httplib::Request& request, httplib::Response& response);
	void ReapExpired();
	bool Authorize(const httplib::Request& request, httplib::Response& response) const;

	struct Session;
	struct Artifact;

	McpApplication& m_application;
	std::string m_token;
	httplib::Server m_server;
	std::thread m_worker;
	std::thread m_reaper;
	std::mutex m_reaperMutex;
	std::condition_variable m_reaperChanged;
	bool m_reaperStopping = false;
	std::atomic<bool> m_workerExited{false};
	std::atomic<unsigned> m_activeDownloads{0};
	std::uint16_t m_port = 0;
	mutable std::mutex m_sessionMutex;
	std::unordered_map<std::string, std::shared_ptr<Session>> m_sessions;
	std::unordered_map<std::string, std::shared_ptr<Artifact>> m_artifacts;
	std::size_t m_activeCalls = 0;
};
} // namespace simxr::app
