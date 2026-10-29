#include "app/UiPch.h"
#include "app/platform/OpenLocalPath.h"
#include "app/platform/windows/LaunchSupport.h"

#include <windows.h>

namespace simxr::app
{
std::string OpenLocalPath(const std::filesystem::path& path)
{
	const auto attributes = GetFileAttributesW(path.c_str());
	if (attributes == INVALID_FILE_ATTRIBUTES)
		return windows::Error("Cannot inspect the requested file or folder").what();
	const bool directory = (attributes & FILE_ATTRIBUTE_DIRECTORY) != 0;
	std::array<wchar_t, 32768> system{};
	const auto length = directory ? GetWindowsDirectoryW(system.data(), static_cast<UINT>(system.size()))
								  : GetSystemDirectoryW(system.data(), static_cast<UINT>(system.size()));
	if (!length || length >= system.size())
		return "Cannot locate the Windows file viewer.";
	const auto viewer = std::filesystem::path(std::wstring_view(system.data(), length)) /
						(directory ? L"explorer.exe" : L"notepad.exe");
	auto command = windows::Quote(viewer.native()) + L" " + windows::Quote(path.native());
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	if (!CreateProcessW(viewer.c_str(), command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup,
						&process))
		return windows::Error("Windows could not open the requested file or folder").what();
	const windows::Handle processHandle(process.hProcess), threadHandle(process.hThread);
	return {};
}
} // namespace simxr::app
