#include "app/platform/RunningApps.h"
#include "Pch.h"
#include "app/platform/windows/LaunchSupport.h"
#include "app/platform/windows/RuntimeEndpoints.h"

namespace simxr::app
{
static bool ProbePipe(const std::string& endpoint, std::chrono::milliseconds timeout, std::uint64_t& process)
{
	std::wstring path;
	try
	{
		path = LocalPipePath(endpoint);
	}
	catch (const std::invalid_argument&)
	{
		return false;
	}
	const auto connect = [&] {
		return CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING, FILE_FLAG_OVERLAPPED,
						   nullptr);
	};
	windows::Handle pipe(connect());
	if (pipe.Get() == INVALID_HANDLE_VALUE && GetLastError() == ERROR_PIPE_BUSY)
	{
		const auto wait = static_cast<DWORD>(std::clamp<std::int64_t>(timeout.count(), 1, 2000));
		if (!WaitNamedPipeW(path.c_str(), wait))
			return false;
		pipe = windows::Handle(connect());
	}
	if (pipe.Get() == INVALID_HANDLE_VALUE)
		return false;
	ULONG server = 0;
	if (GetNamedPipeServerProcessId(pipe.Get(), &server))
		process = server;
	return true;
}

static void ProcessDetails(RunningApp& app)
{
	if (!app.process || app.process > MAXDWORD)
		return;
	windows::Handle process(OpenProcess(PROCESS_QUERY_LIMITED_INFORMATION, FALSE, static_cast<DWORD>(app.process)));
	if (!process.Get())
		return;
	std::wstring path(32768, L'\0');
	DWORD size = static_cast<DWORD>(path.size());
	if (QueryFullProcessImageNameW(process.Get(), 0, path.data(), &size))
	{
		app.executable = windows::Utf8(std::wstring_view(path.data(), size));
		const auto slash = app.executable.find_last_of("/\\");
		app.name = app.executable.substr(slash == std::string::npos ? 0 : slash + 1);
	}
	FILETIME created{}, exited{}, kernel{}, user{};
	if (GetProcessTimes(process.Get(), &created, &exited, &kernel, &user))
	{
		const auto ticks = (std::uint64_t(created.dwHighDateTime) << 32) | created.dwLowDateTime;
		constexpr std::uint64_t unixEpoch = 116444736000000000ULL;
		if (ticks >= unixEpoch)
		{
			using Duration = std::chrono::system_clock::duration;
			const std::chrono::duration<long double, Duration::period> elapsed =
				std::chrono::duration<std::uint64_t, std::ratio<1, 10000000>>(ticks - unixEpoch);
			if (elapsed.count() < static_cast<long double>(Duration::max().count()))
				app.started =
					std::chrono::system_clock::time_point(Duration(static_cast<Duration::rep>(elapsed.count())));
		}
	}
}

std::optional<RunningApp> ProbeRunningApp(const std::string& endpoint, std::chrono::milliseconds timeout)
{
	RunningApp app;
	if (!ProbePipe(endpoint, timeout, app.process))
		return {};
	ProcessDetails(app);
	return app;
}

std::optional<LaunchProfile> ReadProcessLaunch(std::uint64_t)
{
	return {};
}
} // namespace simxr::app
