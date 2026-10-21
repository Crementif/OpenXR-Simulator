#pragma once
#include "app/WindowsLaunch.h"
#include <algorithm>
#include <cstdlib>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <vector>

namespace simxr::app
{
#ifdef _WIN32
inline std::wstring LocalPipePath(const std::string& endpoint)
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
inline bool RuntimeEndpointExists(const std::string& endpoint)
{
	const auto path = LocalPipePath(endpoint);
	WIN32_FIND_DATAW found{};
	const auto search = FindFirstFileW(path.c_str(), &found);
	if (search == INVALID_HANDLE_VALUE)
		return false;
	const std::unique_ptr<void, decltype(&FindClose)> searchOwner(search, FindClose);
	return true;
}
#endif
inline std::vector<std::string> DiscoverRuntimeEndpoints(bool includeExplicit = true)
{
	std::vector<std::string> result;
#ifdef _WIN32
	if (includeExplicit)
		if (const auto path = windows::Environment("SIMXR_CONTROL_SOCKET"); !path.empty())
			result.push_back(path);
	WIN32_FIND_DATAW found{};
	const auto search = FindFirstFileW(LR"(\\.\pipe\SimXR.Control.*)", &found);
	if (search != INVALID_HANDLE_VALUE)
	{
		const std::unique_ptr<void, decltype(&FindClose)> searchOwner(search, FindClose);
		do
		{
			const std::wstring_view name(found.cFileName);
			if (name.starts_with(L"SimXR.Control.") &&
				std::all_of(name.begin(), name.end(), [](wchar_t value) { return value > 0 && value < 128; }))
				result.push_back(std::string(R"(\\.\pipe\)") + std::string(name.begin(), name.end()));
			if (result.size() >= 4096)
			{
				throw std::runtime_error("Too many local runtime endpoints");
			}
		} while (FindNextFileW(search, &found));
	}
#else
	if (includeExplicit)
		if (const auto path = std::getenv("SIMXR_CONTROL_SOCKET"))
			result.emplace_back(path);
	std::filesystem::path directory;
	if (const auto data = std::getenv("SIMXR_DATA_DIR"))
		directory = data;
	else if (const auto data = std::getenv("XDG_DATA_HOME"))
		directory = std::filesystem::path(data) / "OpenXR-Simulator";
	else if (const auto home = std::getenv("HOME"))
		directory = std::filesystem::path(home) / ".local/share/OpenXR-Simulator";
	std::error_code error;
	for (std::filesystem::directory_iterator iterator(directory, error), end; !error && iterator != end;
		 iterator.increment(error))
	{
		const auto name = iterator->path().filename().string();
		if (name.starts_with("control.") && name.ends_with(".sock") && iterator->is_socket(error))
			result.push_back(iterator->path().string());
	}
#endif
	std::sort(result.begin(), result.end());
	result.erase(std::unique(result.begin(), result.end()), result.end());
	return result;
}
} // namespace simxr::app
