#pragma once

#include "app/mcp/McpDebug.h"
#include "app/mcp/McpImage.h"
#include "app/services/ApplicationCatalog.h"

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
	TransientDebug(McpApplication& application, std::shared_ptr<McpCancellation> cancellation, double seconds,
				   std::string operation);
	~TransientDebug();
	void Check();
	void Wait();
	McpConnection Connect(const std::string& application);
	control::Message Request(const std::string& connection, control::OpCode opcode, std::vector<control::Field> fields);
	std::shared_ptr<const CachedImage> Render(const std::string& connection, const ImageRenderRequest& request);
	ApplicationSnapshot Application(const std::string& application);

private:
	std::shared_ptr<McpCancellation> Begin();
	void End() noexcept;
	void CheckResponse(const control::Message& response);
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
