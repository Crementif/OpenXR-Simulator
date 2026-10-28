#pragma once

#include "app/mcp/McpDebug.h"

#include <chrono>
#include <memory>
#include <mutex>
#include <stdexcept>
#include <string>
#include <thread>

namespace simxr::app::mcp
{
class TransientError : public std::runtime_error
{
  public:
	TransientError(std::string text, std::uint32_t value = 0) : std::runtime_error(std::move(text)), code(value) {}
	std::uint32_t code;
};

// Owns a short-lived debugger connection and interrupts pending work on deadline.
class TransientDebug
{
  public:
	TransientDebug(McpApplication& application, std::shared_ptr<McpCancellation> cancellation, double seconds, std::string operation);
	~TransientDebug();
	void Check();
	void Wait();
	nlohmann::json Call(const std::string& name, nlohmann::json arguments);
	nlohmann::json Data(const std::string& name, nlohmann::json arguments);
	nlohmann::json Application(const std::string& name, nlohmann::json arguments);
	std::vector<std::byte> Read(const std::string& connection, std::uint64_t ticket, std::uint64_t offset, std::uint64_t length);

  private:
	std::shared_ptr<McpCancellation> Begin();
	void End() noexcept;
	nlohmann::json CheckResult(std::optional<nlohmann::json> result);
	McpApplication& m_application;
	std::shared_ptr<McpCancellation> m_cancellation;
	std::chrono::steady_clock::time_point m_deadline;
	std::string m_operation;
	McpDebug m_debug;
	std::mutex m_mutex;
	std::shared_ptr<McpCancellation> m_active;
	bool m_terminated = false;
	std::jthread m_monitor;
};
}
