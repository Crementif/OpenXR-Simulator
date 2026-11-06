#pragma once

#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace simxr::app
{
inline constexpr std::size_t MaxLaunchEnvironment = 1024;

struct LaunchProfile
{
	std::string name;
	std::string executable;
	std::vector<std::string> arguments;
	std::string workingDirectory;
	std::string runtimeManifest;
	std::vector<std::pair<std::string, std::string>> environment;
	std::string headsetProfile;
	std::string rig;
	std::string privateProtonPrefix;
	bool useBundledRuntime = false;
	bool operator==(const LaunchProfile&) const = default;
};

void ValidateLaunchProfile(const LaunchProfile& profile);
LaunchProfile ResolveLaunchProfile(LaunchProfile profile, const std::filesystem::path& applicationDirectory);
// The Steam app id of a game installed in a Steam library, from that library's app manifests.
std::optional<std::string> SteamAppId(const std::filesystem::path& executable);
std::string LinuxSteamRuntimeOption(const std::filesystem::path& manifest);
enum class LaunchCommandShell
{
	Posix,
	PowerShell
};
std::string ProfileLaunchCommand(const std::filesystem::path& executable, std::string_view profile,
								 LaunchCommandShell shell);
std::string DecodeProfileLaunchName(std::string_view encoded);

enum class LaunchState
{
	Idle,
	Starting,
	Running,
	Exited,
	Failed
};

struct LaunchSnapshot
{
	LaunchState state = LaunchState::Idle;
	std::string profile;
	std::string detail;
	std::uint64_t process = 0;
	int exitCode = 0;
	bool restartPending = false;
};
} // namespace simxr::app
