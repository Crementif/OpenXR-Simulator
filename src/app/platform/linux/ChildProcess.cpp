#include "app/platform/ChildProcess.h"
#include "Pch.h"

#include <cerrno>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <map>
#include <spawn.h>
#include <stdexcept>
#include <sys/wait.h>

extern char** environ;

namespace simxr::app
{
class ChildEnvironment
{
  public:
	explicit ChildEnvironment(const LaunchProfile& profile)
	{
		std::map<std::string, std::string> environment;
		for (auto entry = environ; entry && *entry; ++entry)
		{
			const std::string value(*entry);
			const auto equals = value.find('=');
			if (equals != std::string::npos)
				environment[value.substr(0, equals)] = value.substr(equals + 1);
		}
		for (const auto& [key, value] : profile.environment)
			environment[key] = value;
		if (!profile.headsetProfile.empty())
			environment["SIMXR_HEADSET_PROFILE"] = profile.headsetProfile;
		if (!profile.runtimeManifest.empty())
			environment["XR_RUNTIME_JSON"] = profile.runtimeManifest;
		m_values.reserve(environment.size());
		m_pointers.reserve(environment.size() + 1);
		for (const auto& [key, value] : environment)
			m_values.push_back(key + "=" + value);
		for (auto& value : m_values)
			m_pointers.push_back(value.data());
		m_pointers.push_back(nullptr);
	}
	char* const* Data() const noexcept
	{
		return m_pointers.data();
	}

  private:
	std::vector<std::string> m_values;
	std::vector<char*> m_pointers;
};
class SpawnActions
{
  public:
	explicit SpawnActions(const std::string& directory)
	{
		const auto initialized = posix_spawn_file_actions_init(&m_actions);
		if (initialized)
			throw std::runtime_error(std::string("Cannot prepare launch: ") + std::strerror(initialized));
		if (!directory.empty())
		{
			const auto changed = posix_spawn_file_actions_addchdir_np(&m_actions, directory.c_str());
			if (changed)
			{
				posix_spawn_file_actions_destroy(&m_actions);
				throw std::runtime_error(std::string("Cannot prepare working directory: ") + std::strerror(changed));
			}
		}
	}
	~SpawnActions()
	{
		posix_spawn_file_actions_destroy(&m_actions);
	}
	SpawnActions(const SpawnActions&) = delete;
	SpawnActions& operator=(const SpawnActions&) = delete;
	const posix_spawn_file_actions_t* Get() const noexcept
	{
		return &m_actions;
	}

  private:
	posix_spawn_file_actions_t m_actions;
};
struct ChildProcess::State
{
	pid_t process = 0;
	std::optional<int> exitCode;
};
ChildProcess::ChildProcess() : m_state(std::make_unique<State>()) {}
ChildProcess::~ChildProcess() = default;
std::unique_ptr<ChildProcess> ChildProcess::Launch(const LaunchProfile& profile)
{
	if (!profile.privateProtonPrefix.empty())
		throw std::invalid_argument(
			"Private Proton registration requires the Windows companion running in the selected prefix");
	if (!profile.runtimeManifest.empty())
	{
		auto manifest = std::filesystem::path(profile.runtimeManifest);
		if (manifest.is_relative())
			manifest = std::filesystem::path(profile.workingDirectory) / manifest;
		if (!std::filesystem::is_regular_file(manifest) || !std::ifstream(manifest).good())
			throw std::runtime_error("Runtime manifest is not a readable file: " + manifest.string());
	}
	ChildEnvironment environment(profile);
	auto arguments = profile.arguments;
	arguments.insert(arguments.begin(), profile.executable);
	std::vector<char*> pointers;
	pointers.reserve(arguments.size() + 1);
	for (auto& argument : arguments)
		pointers.push_back(argument.data());
	pointers.push_back(nullptr);
	SpawnActions actions(profile.workingDirectory);
	// Prepare the C++ owner before spawning: successful native acceptance must
	// not be followed by an allocation that can lose the child's observation.
	auto child = std::unique_ptr<ChildProcess>(new ChildProcess);
	const auto result = posix_spawnp(&child->m_state->process, profile.executable.c_str(), actions.Get(), nullptr,
									 pointers.data(), environment.Data());
	if (result)
		throw std::runtime_error(std::string("Launch failed: ") + std::strerror(result));
	return child;
}
std::uint64_t ChildProcess::Id() const noexcept
{
	return static_cast<std::uint64_t>(m_state->process);
}
void ChildProcess::RequestClose()
{
	if (PollExit())
		return;
	// The unreaped child retains its PID until this owner observes exit.
	if (::kill(m_state->process, SIGTERM) != 0 && errno != ESRCH)
		throw std::runtime_error(std::string("Cannot request child close: ") + std::strerror(errno));
}
std::optional<int> ChildProcess::PollExit()
{
	if (m_state->exitCode)
		return m_state->exitCode;
	int status = 0;
	const auto observed = ::waitpid(m_state->process, &status, WNOHANG);
	if (!observed || (observed < 0 && errno == EINTR))
		return {};
	if (observed < 0)
		throw std::runtime_error(std::string("Cannot observe child exit: ") + std::strerror(errno));
	if (WIFEXITED(status))
		m_state->exitCode = WEXITSTATUS(status);
	else if (WIFSIGNALED(status))
		m_state->exitCode = 128 + WTERMSIG(status);
	return m_state->exitCode;
}
} // namespace simxr::app
