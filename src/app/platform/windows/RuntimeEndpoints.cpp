#include "app/platform/windows/RuntimeEndpoints.h"
#include "Pch.h"
#include "app/platform/windows/LaunchSupport.h"
#include "app/services/RuntimeEndpoints.h"

#include <stdexcept>
#include <tlhelp32.h>

namespace simxr::app
{
std::wstring LocalPipePath(const std::string& endpoint)
{
	constexpr std::string_view prefix = R"(\\.\pipe\SimXR.Control.)";
	if (!endpoint.starts_with(prefix) || endpoint.size() <= prefix.size() || endpoint.size() > 240)
		throw std::invalid_argument("Expected a local \\\\.\\pipe\\SimXR.Control.NAME endpoint");
	for (const unsigned char value : std::string_view(endpoint).substr(prefix.size()))
		if (!((value >= 'a' && value <= 'z') || (value >= 'A' && value <= 'Z') || (value >= '0' && value <= '9') ||
			  value == '.' || value == '-' || value == '_'))
			throw std::invalid_argument("Invalid local control pipe name");
	return std::wstring(endpoint.begin(), endpoint.end());
}
bool RuntimeEndpointExists(const std::string& endpoint)
{
	const auto path = LocalPipePath(endpoint);
	WIN32_FIND_DATAW found{};
	const auto search = FindFirstFileW(path.c_str(), &found);
	if (search == INVALID_HANDLE_VALUE)
	{
		const auto error = GetLastError();
		if (error == ERROR_FILE_NOT_FOUND || error == ERROR_PATH_NOT_FOUND || error == ERROR_NO_MORE_FILES)
			return false;
		if (error == ERROR_ACCESS_DENIED)
			return true;
		if (error != ERROR_NOT_SUPPORTED)
			throw std::runtime_error("Cannot inspect local runtime endpoint: " + std::to_string(error));
		// Wine cannot enumerate pipes. Availability queries do not consume a connection.
		if (WaitNamedPipeW(path.c_str(), 1))
			return true;
		const auto waitError = GetLastError();
		if (waitError == ERROR_FILE_NOT_FOUND || waitError == ERROR_PATH_NOT_FOUND)
			return false;
		if (waitError == ERROR_SEM_TIMEOUT || waitError == ERROR_PIPE_BUSY || waitError == ERROR_ACCESS_DENIED)
			return true;
		throw std::runtime_error("Cannot inspect local runtime endpoint: " + std::to_string(waitError));
	}
	const std::unique_ptr<void, decltype(&FindClose)> searchOwner(search, FindClose);
	return true;
}
std::vector<std::string> DiscoverRuntimeEndpoints(bool includeExplicit)
{
	std::vector<std::string> result;
	if (includeExplicit)
		if (const auto path = windows::Environment("SIMXR_CONTROL_SOCKET"); !path.empty())
			result.push_back(path);
	WIN32_FIND_DATAW found{};
	const auto search = FindFirstFileW(LR"(\\.\pipe\SimXR.Control.*)", &found);
	const auto searchError = search == INVALID_HANDLE_VALUE ? GetLastError() : ERROR_SUCCESS;
	if (search != INVALID_HANDLE_VALUE)
	{
		const std::unique_ptr<void, decltype(&FindClose)> searchOwner(search, FindClose);
		do
		{
			const std::wstring_view name(found.cFileName);
			if (name.starts_with(L"SimXR.Control.") && std::all_of(name.begin(), name.end(),
																   [](wchar_t value)
																   {
																	   return value > 0 && value < 128;
																   }))
				result.push_back(std::string(R"(\\.\pipe\)") + std::string(name.begin(), name.end()));
			if (result.size() >= 4096)
			{
				throw std::runtime_error("Too many local runtime endpoints");
			}
		} while (FindNextFileW(search, &found));
		if (GetLastError() != ERROR_NO_MORE_FILES)
			throw windows::Error("Cannot enumerate runtime endpoints");
	}
	else if (searchError == ERROR_NOT_SUPPORTED)
	{
		// Default endpoints contain the runtime PID; custom names remain explicitly addressable.
		const windows::Handle processes(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
		if (processes.Get() == INVALID_HANDLE_VALUE)
			throw std::runtime_error("Cannot discover runtime processes");
		PROCESSENTRY32W process{};
		process.dwSize = sizeof(process);
		if (Process32FirstW(processes.Get(), &process))
		{
			do
			{
				const auto endpoint = std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(process.th32ProcessID);
				if (RuntimeEndpointExists(endpoint))
					result.push_back(endpoint);
				if (result.size() >= 4096)
					throw std::runtime_error("Too many local runtime endpoints");
			} while (Process32NextW(processes.Get(), &process));
		}
		if (GetLastError() != ERROR_NO_MORE_FILES)
			throw std::runtime_error("Cannot enumerate runtime processes");
	}
	else if (searchError != ERROR_FILE_NOT_FOUND && searchError != ERROR_PATH_NOT_FOUND &&
			 searchError != ERROR_NO_MORE_FILES)
		throw windows::Error("Cannot discover runtime endpoints", searchError);
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}
} // namespace simxr::app
