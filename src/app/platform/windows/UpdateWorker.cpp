#include "app/UiPch.h"
#include "app/platform/windows/LaunchSupport.h"
#include "app/platform/windows/UpdateTransport.h"

namespace simxr::app::windows
{
class UpdateInheritedHandles
{
  public:
	explicit UpdateInheritedHandles(const std::array<HANDLE, 2>& handles)
	{
		SIZE_T size = 0;
		InitializeProcThreadAttributeList(nullptr, 1, 0, &size);
		if (!size)
			throw Error("Cannot allocate update worker attributes");
		m_storage.resize(size);
		m_attributes = reinterpret_cast<LPPROC_THREAD_ATTRIBUTE_LIST>(m_storage.data());
		if (!InitializeProcThreadAttributeList(m_attributes, 1, 0, &size))
		{
			m_attributes = nullptr;
			throw Error("Cannot initialize update worker attributes");
		}
		if (!UpdateProcThreadAttribute(m_attributes, 0, PROC_THREAD_ATTRIBUTE_HANDLE_LIST,
									   const_cast<HANDLE*>(handles.data()), sizeof(handles), nullptr, nullptr))
		{
			const auto failure = Error("Cannot set update worker handles");
			DeleteProcThreadAttributeList(m_attributes);
			m_attributes = nullptr;
			throw failure;
		}
	}
	~UpdateInheritedHandles()
	{
		if (m_attributes)
			DeleteProcThreadAttributeList(m_attributes);
	}
	UpdateInheritedHandles(const UpdateInheritedHandles&) = delete;
	UpdateInheritedHandles& operator=(const UpdateInheritedHandles&) = delete;
	LPPROC_THREAD_ATTRIBUTE_LIST Get() const noexcept
	{
		return m_attributes;
	}

  private:
	std::vector<std::byte> m_storage;
	LPPROC_THREAD_ATTRIBUTE_LIST m_attributes = nullptr;
};
class UpdateChild
{
  public:
	explicit UpdateChild(PROCESS_INFORMATION info) : m_process(info.hProcess), m_thread(info.hThread)
	{
	}
	~UpdateChild()
	{
		if (!m_finished && WaitForSingleObject(m_process.Get(), 0) == WAIT_TIMEOUT)
		{
			TerminateProcess(m_process.Get(), 1);
			WaitForSingleObject(m_process.Get(), 2000);
		}
	}
	HANDLE Process() const noexcept
	{
		return m_process.Get();
	}
	HANDLE Thread() const noexcept
	{
		return m_thread.Get();
	}
	void Finished() noexcept
	{
		m_finished = true;
	}

  private:
	Handle m_process, m_thread;
	bool m_finished = false;
};
std::string RunUpdateWorker(std::wstring_view argument, std::wstring_view target)
{
	std::array<wchar_t, 32768> executable{};
	std::wstring executablePath(target);
	if (executablePath.empty())
	{
		const auto length = GetModuleFileNameW(nullptr, executable.data(), static_cast<DWORD>(executable.size()));
		if (!length || length >= executable.size())
			throw Error("Cannot locate update worker");
		executablePath.assign(executable.data(), length);
	}
	SECURITY_ATTRIBUTES security{sizeof(security), nullptr, TRUE};
	HANDLE rawRead = nullptr, rawWrite = nullptr;
	if (!CreatePipe(&rawRead, &rawWrite, &security, 0))
		throw Error("Cannot create update worker pipe");
	Handle read(rawRead), write(rawWrite);
	if (!SetHandleInformation(read.Get(), HANDLE_FLAG_INHERIT, 0))
		throw Error("Cannot protect update worker pipe");
	Handle nullFile(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE,
							   &security, OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (nullFile.Get() == INVALID_HANDLE_VALUE)
		throw Error("Cannot open update worker input");
	const std::array<HANDLE, 2> handles{write.Get(), nullFile.Get()};
	UpdateInheritedHandles attributes(handles);
	Handle job(CreateJobObjectW(nullptr, nullptr));
	if (!job.Get())
		throw Error("Cannot create update worker job");
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION limits{};
	limits.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE;
	if (!SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &limits, sizeof(limits)))
		throw Error("Cannot configure update worker job");
	STARTUPINFOEXW startup{};
	startup.StartupInfo.cb = sizeof(startup);
	startup.StartupInfo.dwFlags = STARTF_USESTDHANDLES;
	startup.StartupInfo.hStdInput = nullFile.Get();
	startup.StartupInfo.hStdOutput = write.Get();
	startup.StartupInfo.hStdError = nullFile.Get();
	startup.lpAttributeList = attributes.Get();
	auto command = Quote(executablePath) + L" " + Quote(argument);
	PROCESS_INFORMATION information{};
	if (!CreateProcessW(executablePath.c_str(), command.data(), nullptr, nullptr, TRUE,
						EXTENDED_STARTUPINFO_PRESENT | CREATE_NO_WINDOW | CREATE_SUSPENDED, nullptr, nullptr,
						&startup.StartupInfo, &information))
		throw Error("Cannot start update worker");
	UpdateChild child(information);
	if (!AssignProcessToJobObject(job.Get(), child.Process()))
		throw Error("Cannot contain update worker");
	if (ResumeThread(child.Thread()) == static_cast<DWORD>(-1))
		throw Error("Cannot start update worker thread");
	write = Handle{};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(8);
	std::string response;
	std::array<char, 8192> buffer{};
	for (;;)
	{
		if (std::chrono::steady_clock::now() >= deadline)
			throw std::runtime_error("Update check timed out");
		DWORD available = 0;
		if (!PeekNamedPipe(read.Get(), nullptr, 0, nullptr, &available, nullptr) && GetLastError() != ERROR_BROKEN_PIPE)
			throw Error("Cannot inspect update worker output");
		if (available)
		{
			DWORD count = 0;
			const auto capacity = static_cast<DWORD>(std::min<std::size_t>(available, buffer.size()));
			if (!ReadFile(read.Get(), buffer.data(), capacity, &count, nullptr))
				throw Error("Cannot read update worker output");
			if (count > 1024 * 1024 - response.size())
				throw std::runtime_error("Release response is too large");
			response.append(buffer.data(), count);
			continue;
		}
		const auto status = WaitForSingleObject(child.Process(), 0);
		if (status == WAIT_OBJECT_0)
		{
			DWORD remaining = 0;
			if (!PeekNamedPipe(read.Get(), nullptr, 0, nullptr, &remaining, nullptr) && GetLastError() != ERROR_BROKEN_PIPE)
				throw Error("Cannot inspect update worker output");
			if (remaining)
				continue;
			DWORD exitCode = 1;
			if (!GetExitCodeProcess(child.Process(), &exitCode))
				throw Error("Cannot inspect update worker");
			child.Finished();
			if (exitCode != 0 || response.empty())
				throw std::runtime_error("Update check failed");
			return response;
		}
		if (status == WAIT_FAILED)
			throw Error("Cannot wait for update worker");
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	}
}
} // namespace simxr::app::windows
