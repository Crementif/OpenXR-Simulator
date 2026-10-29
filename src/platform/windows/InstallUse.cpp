#include "Pch.h"
#include "platform/windows/InstallUse.h"

#include <cwchar>

namespace simxr::windows
{
bool AcquireInstallationUse(HMODULE module, HANDLE& handle) noexcept
{
	handle = INVALID_HANDLE_VALUE;
	std::array<wchar_t, 32768> path{};
	const auto count = GetModuleFileNameW(module, path.data(), static_cast<DWORD>(path.size()));
	if (!count || count >= path.size())
		return false;
	auto* slash = std::wcsrchr(path.data(), L'\\');
	if (!slash)
		return false;
	*slash = L'\0';
	slash = std::wcsrchr(path.data(), L'\\');
	if (!slash)
		return false;
	*slash = L'\0';
	static constexpr wchar_t suffix[] = L"\\.simxr-use-lock";
	const auto length = std::wcslen(path.data());
	if (length + std::size(suffix) > path.size())
		return false;
	std::wmemcpy(path.data() + length, suffix, std::size(suffix));
	handle = CreateFileW(path.data(), GENERIC_READ, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr,
						 OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr);
	if (handle == INVALID_HANDLE_VALUE)
	{
		const auto error = GetLastError();
		if (error != ERROR_FILE_NOT_FOUND && error != ERROR_PATH_NOT_FOUND)
			return false;
		static constexpr wchar_t marker[] = L"\\.simxr-install";
		std::wmemcpy(path.data() + length, marker, std::size(marker));
		if (GetFileAttributesW(path.data()) != INVALID_FILE_ATTRIBUTES)
			return false;
		const auto markerError = GetLastError();
		return markerError == ERROR_FILE_NOT_FOUND || markerError == ERROR_PATH_NOT_FOUND;
	}
	OVERLAPPED overlap{};
	if (!LockFileEx(handle, LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap))
	{
		CloseHandle(handle);
		handle = INVALID_HANDLE_VALUE;
		return false;
	}
	return true;
}

InstallUse::InstallUse() noexcept : m_ready(AcquireInstallationUse(nullptr, m_handle)) {}

InstallUse::~InstallUse()
{
	if (m_handle != INVALID_HANDLE_VALUE)
		CloseHandle(m_handle);
}

bool InstallUse::Ready() const noexcept
{
	return m_ready;
}
}
