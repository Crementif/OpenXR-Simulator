#include "../TestSupport.h"
#include "app/ProcessLauncher.h"
#include "app/ProcessSupervisor.h"
#include "app/RuntimeEndpoints.h"
#include "app/WindowsLaunch.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <vector>

namespace
{
using namespace simxr::app;
using namespace std::chrono_literals;
std::string Utf8(const std::filesystem::path& path)
{
	return windows::Utf8(path.native());
}
const std::vector<std::wstring> arguments = {L"",
											 L"two words",
											 L"literal $(never-execute) ; & | >",
											 L"embedded\"quote",
											 L"\\",
											 L"tail\\\\",
											 L"space and trailing\\",
											 L"before\\\"quote",
											 L"before\\\\\"quote",
											 L"\"quoted\"",
											 L"line\nbreak",
											 L"日本語 Ελληνικά 🥽"};
struct Directory
{
	std::filesystem::path path = std::filesystem::temp_directory_path() /
								 (L"SimXR launch 日本語 " + std::to_wstring(GetCurrentProcessId()) + L" " +
								  std::to_wstring(std::chrono::steady_clock::now().time_since_epoch().count()));
	Directory()
	{
		std::filesystem::create_directories(path);
	}
	~Directory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}
};
LaunchSnapshot Terminal(ProcessLauncher& launcher)
{
	LaunchSnapshot state;
	Eventually(
		[&]
		{
			state = launcher.Snapshot();
			return state.state == LaunchState::Exited || state.state == LaunchState::Failed;
		},
		"Child did not finish");
	return state;
}
std::filesystem::path Executable()
{
	std::wstring path(32768, L'\0');
	const auto size = GetModuleFileNameW(nullptr, path.data(), static_cast<DWORD>(path.size()));
	Require(size && size < path.size(), "Resolve test executable");
	path.resize(size);
	return path;
}
struct ParentEnvironment
{
	std::wstring name, value;
	bool exists = false;
	explicit ParentEnvironment(const wchar_t* key) : name(key)
	{
		SetLastError(ERROR_SUCCESS);
		const auto size = GetEnvironmentVariableW(key, nullptr, 0);
		exists = size || GetLastError() != ERROR_ENVVAR_NOT_FOUND;
		if (size)
		{
			value.resize(size);
			value.resize(GetEnvironmentVariableW(key, value.data(), size));
		}
	}
	~ParentEnvironment()
	{
		SetEnvironmentVariableW(name.c_str(), exists ? value.c_str() : nullptr);
	}
};
void DiscoveryTests()
{
	const auto endpoint =
		std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(GetCurrentProcessId()) + ".LaunchFixture";
	const auto path = LocalPipePath(endpoint);
	const windows::Handle pipe(CreateNamedPipeW(
		path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1024, 1024, 0, nullptr));
	Require(pipe.Get() != INVALID_HANDLE_VALUE, "Create local discovery fixture");
	Require(RuntimeEndpointExists(endpoint), "Exact local named pipe must be discoverable");
	const auto endpoints = DiscoverRuntimeEndpoints(false);
	Require(std::find(endpoints.begin(), endpoints.end(), endpoint) != endpoints.end(),
			"Runtime discovery must include actual native pipe paths");
}
void LaunchTests()
{
	Directory directory;
	ParentEnvironment restore(L"SIMXR_LAUNCH_TEST");
	ParentEnvironment settingsPath(L"SIMXR_COMPANION_SETTINGS");
	const auto settings = directory.path / L"config 日本語.settings";
	Require(SetEnvironmentVariableW(L"SIMXR_COMPANION_SETTINGS", settings.c_str()), "Set Unicode settings path");
	Require(CompanionSettingsPath() == settings, "Settings path preserves inherited Unicode");
	Require(SetEnvironmentVariableW(L"SIMXR_LAUNCH_TEST", L"parent remains unchanged"), "Set parent sentinel");
	const auto originalRuntime = windows::Environment("XR_RUNTIME_JSON");
	LaunchProfile profile;
	profile.name = "Unicode literal launch";
	// Exercise spaces and Unicode in the executable's own path as well as cwd and argv.
	const auto executable = directory.path / L"child 日本語 with spaces.exe";
	std::filesystem::copy_file(Executable(), executable);
	profile.executable = Utf8(executable);
	profile.workingDirectory = Utf8(directory.path);
	profile.runtimeManifest = Utf8(directory.path / L"runtime 日本語.json");
	profile.headsetProfile = "quest3";
	profile.environment = {
		{"simxr_launch_test", "child 日本語"}, {"SIMXR_SEED", "987654321"}, {"WINEPREFIX", "C:\\prefix 日本語\\"}};
	profile.arguments.push_back("--literal-child");
	for (const auto& argument : arguments)
		profile.arguments.push_back(windows::Utf8(argument));
	ProcessSupervisor processes;
	ProcessLauncher launcher(processes);
	Require(launcher.Launch(profile), "Admit Windows launch");
	const auto state = Terminal(launcher);
	Require(state.state == LaunchState::Exited && state.exitCode == 37 && state.process,
			"Child must verify literal argv, Unicode environment and cwd");
	Require(windows::Environment("SIMXR_LAUNCH_TEST") == "parent remains unchanged" &&
				windows::Environment("XR_RUNTIME_JSON") == originalRuntime,
			"Launch must not mutate parent environment");
	profile.executable = Utf8(directory.path / L"missing.exe");
	Require(launcher.Launch(profile), "Admit missing executable");
	const auto missing = Terminal(launcher);
	Require(missing.state == LaunchState::Failed && missing.detail.find("Launch failed") != std::string::npos,
			"Report CreateProcess error");
	profile.executable = Utf8(executable);
	profile.workingDirectory = Utf8(directory.path / L"missing directory");
	Require(launcher.Launch(profile), "Admit invalid cwd");
	Require(Terminal(launcher).state == LaunchState::Failed, "Invalid cwd must fail");
	profile.workingDirectory = Utf8(directory.path);
	profile.arguments = {std::string(32767, 'x')};
	Require(launcher.Launch(profile), "Admit oversized argv for async validation");
	Require(Terminal(launcher).state == LaunchState::Failed, "Oversized Windows command line must fail");
	profile.arguments = {std::string("invalid\xff", 8)};
	Require(launcher.Launch(profile), "Admit malformed UTF-8 for async validation");
	Require(Terminal(launcher).state == LaunchState::Failed,
			"Malformed UTF-8 must not be interpreted in the system code page");
	profile.arguments = {"--surviving-child"};
	DWORD pid = 0;
	std::chrono::steady_clock::time_point closing;
	{
		ProcessLauncher observer(processes);
		Require(observer.Launch(profile), "Admit independent application");
		Eventually(
			[&]
			{
				const auto launched = observer.Snapshot();
				pid = static_cast<DWORD>(launched.process);
				return launched.state == LaunchState::Running;
			},
			"Observe child PID");
		closing = std::chrono::steady_clock::now();
	}
	Require(std::chrono::steady_clock::now() - closing < 1s,
			"Closing launcher must join promptly without waiting for child exit");
	windows::Handle process(OpenProcess(SYNCHRONIZE | PROCESS_QUERY_LIMITED_INFORMATION, FALSE, pid));
	Require(process.Get() && WaitForSingleObject(process.Get(), 0) == WAIT_TIMEOUT,
			"Closing observer must leave child alive");
	Require(WaitForSingleObject(process.Get(), 5000) == WAIT_OBJECT_0, "Independent child must finish");
	Require(std::filesystem::exists(directory.path / L"survived.txt"),
			"Child must continue after launcher destruction");
}
int Child(int argc, wchar_t** argv)
{
	Require(static_cast<std::size_t>(argc) == arguments.size() + 2, "Child argument count");
	for (std::size_t index = 0; index < arguments.size(); ++index)
		Require(std::wstring_view(argv[index + 2]) == arguments[index],
				"Actual CRT argv must preserve the literal argument");
	Require(windows::Environment("SIMXR_LAUNCH_TEST") == "child 日本語",
			"Case-insensitive child Unicode environment override");
	Require(windows::Environment("SIMXR_SEED") == "987654321", "Seed environment");
	Require(windows::Environment("WINEPREFIX") == "C:\\prefix 日本語\\", "Prefix environment");
	Require(windows::Environment("SIMXR_HEADSET_PROFILE") == "quest3", "Headset environment");
	Require(windows::Environment("XR_RUNTIME_JSON") == Utf8(std::filesystem::current_path() / L"runtime 日本語.json"),
			"Unicode cwd and child-only runtime manifest");
	return 37;
}
} // namespace
int wmain(int argc, wchar_t** argv)
{
	try
	{
		if (argc > 1 && std::wstring_view(argv[1]) == L"--literal-child")
			return Child(argc, argv);
		if (argc == 2 && std::wstring_view(argv[1]) == L"--surviving-child")
		{
			Sleep(1500);
			std::ofstream(std::filesystem::current_path() / L"survived.txt") << "alive";
			return 0;
		}
		LaunchTests();
		DiscoveryTests();
		bool rejected = false;
		try
		{
			(void)RuntimeEndpointExists(R"(\\remote\pipe\SimXR.Control.123)");
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Discovery must reject remote pipe names");
		std::cout << "Windows launch tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
