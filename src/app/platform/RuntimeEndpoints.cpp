#include "app/services/RuntimeEndpoints.h"
#include "Pch.h"

namespace simxr::app
{
static std::filesystem::path DataDirectory()
{
	if (const auto data = std::getenv("SIMXR_DATA_DIR"); data && *data)
		return data;
	if (const auto data = std::getenv("XDG_DATA_HOME"); data && *data)
		return std::filesystem::path(data) / "OpenXR-Simulator";
	if (const auto home = std::getenv("HOME"); home && *home)
		return std::filesystem::path(home) / ".local/share/OpenXR-Simulator";
	return {};
}

std::vector<std::string> DiscoverRuntimeEndpoints(bool includeExplicit)
{
	std::vector<std::string> result;
	if (includeExplicit)
		if (const auto path = std::getenv("SIMXR_CONTROL_SOCKET"); path && *path)
			result.emplace_back(path);
	const auto directory = DataDirectory();
	if (!directory.empty())
	{
		std::error_code error;
		for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end;
			 entry.increment(error))
		{
			const auto name = entry->path().filename().string();
			if (!name.starts_with("control.") || !name.ends_with(".sock"))
				continue;
			std::error_code statusError;
			const bool socket = entry->is_socket(statusError);
			if (statusError && statusError != std::errc::no_such_file_or_directory)
				throw std::filesystem::filesystem_error("Cannot inspect runtime endpoint", entry->path(), statusError);
			if (socket)
				result.push_back(entry->path().string());
			if (result.size() >= 4096)
				throw std::runtime_error("Too many local runtime endpoints");
		}
		if (error && error != std::errc::no_such_file_or_directory)
			throw std::filesystem::filesystem_error("Cannot discover runtime endpoints", directory, error);
	}
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}
} // namespace simxr::app
