#pragma once

#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>

namespace simxr::app
{
inline constexpr std::uint32_t McpConnectorProtocolVersion = 1;

struct McpEndpoint
{
	std::uint16_t port = 0;
	std::string token;
};

// One lock belongs to an app executable path and remains held by its app process.
class McpInstanceDiscovery
{
  public:
	explicit McpInstanceDiscovery(std::filesystem::path executable, bool windowless = false);
	~McpInstanceDiscovery();
	McpInstanceDiscovery(const McpInstanceDiscovery&) = delete;
	McpInstanceDiscovery& operator=(const McpInstanceDiscovery&) = delete;
	void StartIfNeeded();
	std::optional<McpEndpoint> Read();
	const std::string& StartupError() const noexcept;

  private:
	std::filesystem::path m_executable, m_directory;
	std::string m_key;
	std::string m_startupError;
#ifdef _WIN32
	void* m_child = nullptr;
#else
	int m_child = -1;
#endif
	bool m_observed = false, m_windowless = false;
};

class McpInstanceRegistration
{
  public:
	explicit McpInstanceRegistration(std::intptr_t inheritedHandle);
	~McpInstanceRegistration();
	McpInstanceRegistration(const McpInstanceRegistration&) = delete;
	McpInstanceRegistration& operator=(const McpInstanceRegistration&) = delete;
	void Publish(const McpEndpoint& endpoint);

  private:
	std::filesystem::path m_executable, m_directory;
	std::string m_key;
#ifdef _WIN32
	void* m_lock = nullptr;
#else
	int m_lock = -1;
#endif
};

std::filesystem::path McpOwnExecutable();
} // namespace simxr::app
