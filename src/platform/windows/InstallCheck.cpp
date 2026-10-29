#include "Pch.h"
#include "platform/windows/InstallUse.h"

#include <cwchar>
#include <iostream>
#include <windows.h>
#include <restartmanager.h>

namespace simxr::windows
{
class NativeHandle
{
public:
	explicit NativeHandle(HANDLE handle) noexcept : m_handle(handle) {}

	~NativeHandle()
	{
		Reset();
	}

	NativeHandle(const NativeHandle&) = delete;
	NativeHandle& operator=(const NativeHandle&) = delete;

	HANDLE Get() const noexcept
	{
		return m_handle;
	}

	bool Valid() const noexcept
	{
		return m_handle && m_handle != INVALID_HANDLE_VALUE;
	}

	void Reset() noexcept
	{
		if (Valid())
			CloseHandle(m_handle);
		m_handle = nullptr;
	}

private:
	HANDLE m_handle = nullptr;
};

class RestartSession
{
public:
	RestartSession()
	{
		std::array<wchar_t, CCH_RM_SESSION_KEY + 1> key{};
		const auto error = RmStartSession(&m_session, 0, key.data());
		if (error != ERROR_SUCCESS)
			throw std::system_error(static_cast<int>(error), std::system_category(), "Start Restart Manager session");
		m_open = true;
	}

	~RestartSession()
	{
		if (m_open)
			RmEndSession(m_session);
	}

	RestartSession(const RestartSession&) = delete;
	RestartSession& operator=(const RestartSession&) = delete;

	bool InUse(const std::vector<std::filesystem::path>& files) const
	{
		if (files.empty())
			return false;
		std::vector<LPCWSTR> paths;
		paths.reserve(files.size());
		for (const auto& file : files)
			paths.push_back(file.c_str());
		if (paths.size() > std::numeric_limits<UINT>::max())
			throw std::length_error("Too many installation files");
		auto error =
			RmRegisterResources(m_session, static_cast<UINT>(paths.size()), paths.data(), 0, nullptr, 0, nullptr);
		if (error != ERROR_SUCCESS)
			throw std::system_error(static_cast<int>(error), std::system_category(), "Register installed files");
		UINT needed = 0, count = 0;
		DWORD reasons = 0;
		error = RmGetList(m_session, &needed, &count, nullptr, &reasons);
		if (error == ERROR_MORE_DATA || (error == ERROR_SUCCESS && (needed != 0 || reasons != 0)))
			return true;
		if (error != ERROR_SUCCESS)
			throw std::system_error(static_cast<int>(error), std::system_category(), "Check installed files");
		return false;
	}

private:
	DWORD m_session = 0;
	bool m_open = false;
};

static int Check(const std::filesystem::path& installation)
{
	const auto bin = installation / L"bin";
	std::error_code error;
	if (!std::filesystem::exists(bin, error))
	{
		if (error)
			throw std::system_error(error, "Inspect installation");
		return 0;
	}
	std::vector<std::filesystem::path> files;
	for (const auto& entry : std::filesystem::directory_iterator(bin))
	{
		if (!entry.is_regular_file())
			continue;
		const auto extension = entry.path().extension().wstring();
		if (_wcsicmp(extension.c_str(), L".exe") == 0 || _wcsicmp(extension.c_str(), L".dll") == 0)
			files.push_back(entry.path());
	}
	RestartSession session;
	if (session.InUse(files))
	{
		std::wcerr << L"Close OpenXR Simulator and applications using its runtime, then retry.\n";
		return 2;
	}
	return 0;
}

static void WriteStatus(const std::filesystem::path& path, std::string_view value)
{
	std::ofstream output(path, std::ios::binary | std::ios::trunc);
	output.exceptions(std::ios::badbit | std::ios::failbit);
	output.write(value.data(), static_cast<std::streamsize>(value.size()));
	output.close();
}

static int Guard(const std::filesystem::path& installation, DWORD parentId, const std::filesystem::path& status,
				 const std::filesystem::path& stop)
{
	NativeHandle parent(OpenProcess(SYNCHRONIZE, FALSE, parentId));
	if (!parent.Valid())
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Open installer process");
	NativeHandle file(CreateFileW((installation / L".simxr-use-lock").c_str(), GENERIC_READ | GENERIC_WRITE,
								  FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE, nullptr, OPEN_ALWAYS,
								  FILE_ATTRIBUTE_NORMAL, nullptr));
	if (!file.Valid())
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Open installation lock");
	OVERLAPPED overlap{};
	if (!LockFileEx(file.Get(), LOCKFILE_EXCLUSIVE_LOCK | LOCKFILE_FAIL_IMMEDIATELY, 0, 1, 0, &overlap))
	{
		WriteStatus(status, "busy");
		return 2;
	}
	if (Check(installation) != 0)
	{
		WriteStatus(status, "busy");
		return 2;
	}
	WriteStatus(status, "ready:" + std::to_string(GetCurrentProcessId()));
	for (;;)
	{
		const auto wait = WaitForSingleObject(parent.Get(), 100);
		if (wait == WAIT_OBJECT_0)
			break;
		if (wait != WAIT_TIMEOUT)
			throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Wait for installer");
		std::error_code error;
		if (std::filesystem::exists(stop, error))
			break;
		if (error)
			throw std::system_error(error, "Inspect installer stop signal");
	}
	file.Reset();
	parent.Reset();
	WriteStatus(status, "released");
	return 0;
}
}

int wmain(int argc, wchar_t** argv)
{
	if ((argc != 2 && argc != 6) || !std::filesystem::path(argv[argc == 2 ? 1 : 2]).is_absolute())
	{
		std::wcerr << L"An absolute installation directory is required.\n";
		return 1;
	}
	try
	{
		if (argc == 6)
		{
			if (std::wstring_view(argv[1]) != L"--guard")
				throw std::invalid_argument("Invalid installer guard arguments");
			std::size_t parsed = 0;
			const auto id = std::stoul(argv[3], &parsed);
			if (parsed != std::wcslen(argv[3]) || !id || id > std::numeric_limits<DWORD>::max())
				throw std::invalid_argument("Invalid installer process ID");
			try
			{
				return simxr::windows::Guard(argv[2], static_cast<DWORD>(id), argv[4], argv[5]);
			}
			catch (...)
			{
				simxr::windows::WriteStatus(argv[4], "error");
				throw;
			}
		}
		simxr::windows::InstallUse installUse;
		if (!installUse.Ready())
			return 2;
		return simxr::windows::Check(argv[1]);
	}
	catch (const std::exception& error)
	{
		std::cerr << "Cannot check whether installation files are in use: " << error.what() << '\n';
		return 1;
	}
}
