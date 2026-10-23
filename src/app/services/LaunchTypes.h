#pragma once

#include <cstddef>
#include <cstdint>
#include <string>
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
	bool operator==(const LaunchProfile&) const = default;
};
void ValidateLaunchProfile(const LaunchProfile& profile);

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
};
} // namespace simxr::app
