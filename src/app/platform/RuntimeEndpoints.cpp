#include "app/services/RuntimeEndpoints.h"
#include "Pch.h"
#include "control/RuntimeEndpoint.h"

namespace simxr::app
{
std::vector<std::string> DiscoverRuntimeEndpoints()
{
	std::vector<std::string> result;
	const auto directory = control::RuntimeDataDirectory();
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
	std::sort(result.begin(), result.end());
	return result;
}
} // namespace simxr::app
