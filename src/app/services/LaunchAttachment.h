#pragma once

#include "app/services/LaunchTypes.h"

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <set>
#include <string>
#include <variant>

namespace simxr::app
{
class RuntimeConnection;
// Compute the launched process's local endpoint without waiting for an XR session.
std::string LaunchRuntimeEndpoint(const LaunchProfile& profile, std::uint64_t process);
// Best-effort launch-time environment inference; an application can change it later.
std::optional<std::filesystem::path> LaunchRuntimeDataDirectory(const LaunchProfile& profile);

struct LaunchedSession
{
	std::string endpoint;
	std::uint64_t session = 0;
	std::uint64_t generation = 0;
	std::uint64_t process = 0;
};

// The dedicated discovery worker outlives this UI-thread attachment attempt.
class LaunchAttachment
{
public:
	LaunchAttachment(RuntimeConnection& discovery, const LaunchProfile& profile, std::string replacedEndpoint = {});
	~LaunchAttachment();
	LaunchAttachment(const LaunchAttachment&) = delete;
	LaunchAttachment& operator=(const LaunchAttachment&) = delete;
	void Observe(const LaunchSnapshot& process);
	std::optional<LaunchedSession> Poll();

private:
	struct WaitingForProcess
	{
	};

	struct WaitingForEndpoint
	{
		std::uint64_t process;
	};

	struct Connecting
	{
		std::string endpoint;
		std::uint64_t epoch, process;
	};

	struct WaitingForSession
	{
		std::string endpoint;
		std::uint64_t epoch, process;
		std::optional<std::uint64_t> request;
	};

	struct Complete
	{
	};

	RuntimeConnection& m_discovery;
	std::filesystem::path m_directory;
	std::string m_customEndpoint, m_replacedEndpoint;
	std::string m_lastFailure;
	std::set<std::string> m_previous;
	std::chrono::steady_clock::time_point m_deadline;
	std::chrono::steady_clock::time_point m_retryAfter{};
	std::variant<WaitingForProcess, WaitingForEndpoint, Connecting, WaitingForSession, Complete> m_state;
	std::optional<std::uint64_t> m_process;
	void Initialize(const LaunchProfile& profile);
	std::string ExpectedEndpoint(std::uint64_t process) const;
	bool EndpointExists(const std::string& endpoint) const;
};
} // namespace simxr::app
