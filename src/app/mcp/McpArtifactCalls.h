#pragma once

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>
#include <utility>
#include <vector>
#include <nlohmann/json.hpp>

namespace simxr::app
{
class McpApplication;
class McpCancellation;
class McpDebug;
namespace mcp
{
class ArtifactError : public std::runtime_error
{
  public:
	ArtifactError(std::string message, std::uint32_t value) : std::runtime_error(std::move(message)), code(value) {}
	std::uint32_t code;
};
// A bounded operation on a caller-owned debugger connection. A deadline or
// cancellation closes that connection to interrupt an in-flight transport call.
class ArtifactCalls
{
  public:
	ArtifactCalls(McpApplication& app, McpDebug& debug, std::string connection,
		std::shared_ptr<McpCancellation> cancellation, double seconds, std::string operation);
	~ArtifactCalls();
	ArtifactCalls(const ArtifactCalls&) = delete;
	ArtifactCalls& operator=(const ArtifactCalls&) = delete;
	void Check() const;
	nlohmann::json Call(const char* name, nlohmann::json arguments);
	std::vector<std::byte> Read(std::uint64_t ticket, std::uint64_t offset, std::uint64_t length);

  private:
	std::shared_ptr<McpCancellation> Begin();
	void End() noexcept;
	void Disconnect() noexcept;
	McpApplication& m_app;
	McpDebug& m_debug;
	std::string m_connection;
	std::shared_ptr<McpCancellation> m_cancellation;
	std::chrono::steady_clock::time_point m_deadline;
	std::string m_operation;
	std::mutex m_mutex;
	std::shared_ptr<McpCancellation> m_active;
	bool m_terminated = false;
	std::jthread m_monitor;
};
}
}
