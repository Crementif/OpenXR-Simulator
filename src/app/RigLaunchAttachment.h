#pragma once

#include "app/CompanionSettings.h"
#include "app/LaunchTypes.h"

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
struct RigSessionAttachment
{
	std::string endpoint;
	std::uint64_t session = 0;
	std::uint64_t generation = 0;
};

// The dedicated discovery worker outlives this UI-thread attachment attempt.
class RigLaunchAttachment
{
  public:
	RigLaunchAttachment(RuntimeConnection& discovery, const LaunchProfile& profile);
	~RigLaunchAttachment();
	RigLaunchAttachment(const RigLaunchAttachment&) = delete;
	RigLaunchAttachment& operator=(const RigLaunchAttachment&) = delete;
	void Observe(const LaunchSnapshot& process);
	std::optional<RigSessionAttachment> Poll();

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
		std::uint64_t epoch;
	};
	struct WaitingForSession
	{
		std::string endpoint;
		std::uint64_t epoch;
		std::optional<std::uint64_t> request;
	};
	struct Complete
	{
	};
	RuntimeConnection& m_discovery;
	std::filesystem::path m_directory;
	std::string m_customEndpoint;
	std::set<std::string> m_previous;
	std::chrono::steady_clock::time_point m_deadline;
	std::variant<WaitingForProcess, WaitingForEndpoint, Connecting, WaitingForSession, Complete> m_state;
	std::optional<std::uint64_t> m_process;
	std::string ExpectedEndpoint(std::uint64_t process) const;
	bool EndpointExists(const std::string& endpoint) const;
};
} // namespace simxr::app
