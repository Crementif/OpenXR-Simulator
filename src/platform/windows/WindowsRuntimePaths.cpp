#include "platform/windows/WindowsRuntimePaths.h"
#include "Pch.h"

#include <system_error>
#include <windows.h>

namespace simxr::windows
{
static std::wstring Environment(const wchar_t* name)
{
	std::wstring value;
	for (;;)
	{
		SetLastError(ERROR_SUCCESS);
		const auto count = GetEnvironmentVariableW(name, value.data(), static_cast<DWORD>(value.size()));
		if (!count)
		{
			const auto error = GetLastError();
			if (error != ERROR_SUCCESS && error != ERROR_ENVVAR_NOT_FOUND)
				throw std::system_error(static_cast<int>(error), std::system_category(), "Read runtime environment");
			return {};
		}
		if (count < value.size())
		{
			value.resize(count);
			return value;
		}
		value.resize(count);
	}
}

const std::filesystem::path& RuntimeDataDirectory()
{
	static const auto directory = []
	{
		if (const auto explicitPath = Environment(L"SIMXR_DATA_DIR"); !explicitPath.empty())
			return std::filesystem::path(explicitPath);
		const auto base = Environment(L"LOCALAPPDATA");
		return std::filesystem::path(base.empty() ? L"." : base) / L"OpenXR-Simulator";
	}();
	return directory;
}
} // namespace simxr::windows
