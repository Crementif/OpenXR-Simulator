#pragma once

#include <cstdint>
#include <string>

namespace simxr::app
{
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
