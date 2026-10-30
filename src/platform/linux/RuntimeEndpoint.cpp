#include "control/RuntimeEndpoint.h"
#include "Pch.h"

namespace simxr::control
{
std::filesystem::path RuntimeDataDirectory()
{
	if (const char* path = std::getenv("SIMXR_DATA_DIR"); path && *path)
		return path;
	if (const char* base = std::getenv("XDG_DATA_HOME"); base && *base)
		return std::filesystem::path(base) / "OpenXR-Simulator";
	const char* home = std::getenv("HOME");
	return std::filesystem::path(home ? home : ".") / ".local/share/OpenXR-Simulator";
}

std::string RuntimeEndpoint(std::uint64_t process, const std::filesystem::path& directory)
{
	return (directory / ("control." + std::to_string(process) + ".sock")).string();
}

std::string RuntimeEndpoint(std::uint64_t process)
{
	return RuntimeEndpoint(process, RuntimeDataDirectory());
}
} // namespace simxr::control
