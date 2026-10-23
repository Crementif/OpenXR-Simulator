#include "Pch.h"
#include "app/platform/FileStorage.h"
#include "app/services/CompanionSettings.h"
#include "app/services/Utf8Path.h"
#include "app/platform/windows/LaunchSupport.h"

#include <atomic>
#include <stdexcept>

namespace simxr::app
{
static std::filesystem::path Absolute(const std::string& value)
{
	auto path = Utf8Path(value);
	if (!path.is_absolute())
		throw std::runtime_error("Companion configuration path must be absolute");
	return path;
}
std::filesystem::path CompanionSettingsPath()
{
	if (const auto path = windows::Environment("SIMXR_COMPANION_SETTINGS"); !path.empty())
		return Absolute(path);
	if (const auto path = windows::Environment("XDG_CONFIG_HOME"); !path.empty())
		return Absolute(path) / "OpenXR-Simulator/companion.settings";
	if (const auto path = windows::Environment("APPDATA"); !path.empty())
		return Absolute(path) / "OpenXR-Simulator/companion.settings";
	if (const auto path = windows::Environment("HOME"); !path.empty())
		return Absolute(path) / ".config/OpenXR-Simulator/companion.settings";
	throw std::runtime_error("No absolute companion configuration directory is available");
}
std::filesystem::path CreatePrivateTemporaryFile(const std::filesystem::path& destination)
{
	static std::atomic<std::uint64_t> s_sequence{0};
	for (unsigned attempt = 0; attempt < 64; ++attempt)
	{
		auto temporary = destination;
		temporary += L"." + std::to_wstring(GetCurrentProcessId()) + L"." +
			std::to_wstring(s_sequence.fetch_add(1, std::memory_order_relaxed)) + L".tmp";
		const HANDLE file = CreateFileW(temporary.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_NEW,
			FILE_ATTRIBUTE_NORMAL, nullptr);
		if (file == INVALID_HANDLE_VALUE)
		{
			if (GetLastError() == ERROR_FILE_EXISTS || GetLastError() == ERROR_ALREADY_EXISTS)
				continue;
			throw windows::Error("Cannot create temporary file");
		}
		if (!CloseHandle(file))
		{
			const auto failure = windows::Error("Cannot close temporary file");
			DeleteFileW(temporary.c_str());
			throw failure;
		}
		return temporary;
	}
	throw std::runtime_error("Too many existing temporary files");
}
void CommitTemporaryFile(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
	if (!MoveFileExW(temporary.c_str(), destination.c_str(), MOVEFILE_REPLACE_EXISTING | MOVEFILE_WRITE_THROUGH))
		throw windows::Error("Cannot replace destination file");
}
bool CanExecuteFile(const std::filesystem::path&)
{
	return true;
}
}
