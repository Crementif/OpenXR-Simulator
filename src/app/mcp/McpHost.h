#pragma once

#include "app/mcp/McpUiState.h"
#include "app/mcp/McpInstance.h"

#include <atomic>
#include <cstdint>
#include <memory>
#include <optional>
#include <string>
#include <thread>

namespace simxr::app
{
class ApplicationCatalog;

// Owns the app's MCP service and retires its listener without blocking the UI thread.
class McpHost
{
public:
	McpHost(bool preference, std::optional<std::uint16_t> initialPort, std::string initialToken,
			std::intptr_t bootstrapDescriptor, std::unique_ptr<McpInstanceRegistration> startupRegistration = {},
			bool windowless = false);
	~McpHost();
	McpHost(const McpHost&) = delete;
	McpHost& operator=(const McpHost&) = delete;
	void SetPreference(bool enabled);
	void StopForSession();
	void Retry();
	void Update();
	void Poll(ApplicationCatalog& applications);
	McpUiState State() const;

private:
	struct Running;
	void Start();
	void Retire();
	std::unique_ptr<Running> m_running;
	std::unique_ptr<McpInstanceRegistration> m_startupRegistration;
	std::thread m_stopper;
	std::atomic<bool> m_stopped{false};
	std::optional<std::uint16_t> m_initialPort;
	std::string m_initialToken, m_error, m_configuration, m_codexConfiguration, m_configurationError;
	std::intptr_t m_bootstrapDescriptor;
	bool m_preference, m_desired, m_sessionOverride, m_initialPending, m_windowless;
};
} // namespace simxr::app
