#include "../TestSupport.h"
#include "app/services/CompanionSettings.h"
#include "app/services/ProcessLauncher.h"
#include "app/services/ProcessSupervisor.h"
#include "app/services/RuntimeEndpoints.h"
#include "app/platform/windows/RuntimeEndpoints.h"
#include "app/platform/windows/LaunchSupport.h"

#include <array>
#include <chrono>
#include <cstddef>
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
struct RuntimeRegistration
{
	bool exists = false;
	DWORD type = 0;
	std::vector<std::byte> bytes;
	bool operator==(const RuntimeRegistration&) const = default;
};
RuntimeRegistration ReadRuntimeRegistration(HKEY root)
{
	constexpr auto flags = RRF_RT_ANY | RRF_NOEXPAND | RRF_SUBKEY_WOW6464KEY;
	constexpr auto key = L"SOFTWARE\\Khronos\\OpenXR\\1";
	RuntimeRegistration result;
	DWORD size = 0;
	auto status = RegGetValueW(root, key, L"ActiveRuntime", flags, &result.type, nullptr, &size);
	if (status == ERROR_FILE_NOT_FOUND || status == ERROR_PATH_NOT_FOUND)
		return result;
	Require(status == ERROR_SUCCESS, "Read runtime registration size");
	result.exists = true;
	result.bytes.resize(size);
	status = RegGetValueW(root, key, L"ActiveRuntime", flags, &result.type, result.bytes.data(), &size);
	Require(status == ERROR_SUCCESS && size == result.bytes.size(), "Read unchanged runtime registration");
	return result;
}
RuntimeRegistration ManifestRegistration(const std::filesystem::path& path)
{
	const auto canonical = std::filesystem::canonical(path).native();
	const auto* begin = reinterpret_cast<const std::byte*>(canonical.c_str());
	return {true, REG_SZ, {begin, begin + (canonical.size() + 1) * sizeof(wchar_t)}};
}
void PrivatePrefixTests()
{
	Directory directory;
	const auto invalidManifest = directory.path / L"invalid runtime 日本語.json";
	{
		std::ofstream output(invalidManifest, std::ios::binary);
		output << "{}";
		output.close();
		Require(output.good(), "Write invalid runtime fixture");
	}
	const auto unowned = directory.path / L"unowned" / L"pfx";
	std::filesystem::create_directories(unowned);
	LaunchProfile profile;
	profile.name = "Private prefix launch";
	profile.executable = Utf8(Executable());
	profile.workingDirectory = Utf8(directory.path);
	profile.arguments = {"--private-prefix-child"};
	profile.runtimeManifest = Utf8(invalidManifest);
	profile.privateProtonPrefix = Utf8(unowned);
	ProcessSupervisor processes;
	ProcessLauncher launcher(processes);
	const auto reject = [&](const LaunchProfile& requested, const char* reason)
	{
		const auto machine = ReadRuntimeRegistration(HKEY_LOCAL_MACHINE);
		const auto user = ReadRuntimeRegistration(HKEY_CURRENT_USER);
		Require(launcher.Launch(requested), "Admit private-prefix launch for validation");
		const auto state = Terminal(launcher);
		Require(ReadRuntimeRegistration(HKEY_LOCAL_MACHINE) == machine &&
					ReadRuntimeRegistration(HKEY_CURRENT_USER) == user,
				"Rejected private-prefix launch must preserve both runtime registrations");
		Require(state.state == LaunchState::Failed && !state.process,
				"Rejected private-prefix launch must not spawn a child");
		if (state.detail.find(reason) == std::string::npos)
			throw std::runtime_error("Unexpected private-prefix refusal: " + state.detail);
	};
	const auto ntdll = GetModuleHandleW(L"ntdll.dll");
	const bool wine = ntdll && GetProcAddress(ntdll, "wine_get_version");
	reject(profile, wine ? "ownership marker is missing" : "only inside Wine/Proton");

	const auto selected = windows::Environment("SIMXR_PRIVATE_PROTON_TEST_PREFIX");
	const auto other = windows::Environment("SIMXR_PRIVATE_PROTON_TEST_OTHER_PREFIX");
	const auto manifest = windows::Environment("SIMXR_PRIVATE_PROTON_TEST_MANIFEST");
	if (selected.empty() && other.empty() && manifest.empty())
	{
		std::cout << "Private Proton prefix qualification NOT EXERCISED: supply SIMXR_PRIVATE_PROTON_TEST_PREFIX, "
					 "SIMXR_PRIVATE_PROTON_TEST_OTHER_PREFIX and SIMXR_PRIVATE_PROTON_TEST_MANIFEST\n";
		return;
	}
	Require(wine && !selected.empty() && !other.empty() && !manifest.empty(),
			"Private-prefix qualification needs Wine and all three provisioned fixture paths");
	for (const auto& text : {selected, other})
	{
		const std::filesystem::path prefix(windows::Wide(text));
		Require(std::filesystem::is_regular_file(prefix / L".simxr-private-proton") &&
					std::filesystem::is_regular_file(prefix.parent_path() / L".simxr-private-proton"),
				"Private-prefix fixture must already have both provisioner ownership markers");
	}
	profile.runtimeManifest = manifest;
	profile.privateProtonPrefix = other;
	reject(profile, "not the companion's current prefix");
	profile.privateProtonPrefix = selected;
	profile.runtimeManifest = Utf8(invalidManifest);
	reject(profile, "requires the generated simulator runtime manifest");
	profile.runtimeManifest = manifest;
	for (const auto* variable : {"wineprefix", "Steam_Compat_Data_Path"})
	{
		profile.environment = {{variable, other}};
		reject(profile, "cannot override");
	}
	profile.environment.clear();
	const auto executable = profile.executable;
	profile.executable = Utf8(directory.path / L"missing.exe");
	reject(profile, "executable");
	profile.executable = executable;
	profile.workingDirectory = Utf8(directory.path / L"missing directory");
	reject(profile, "directory");
	profile.workingDirectory = Utf8(directory.path);

	ParentEnvironment runtimeEnvironment(L"XR_RUNTIME_JSON");
	Require(SetEnvironmentVariableW(L"XR_RUNTIME_JSON", L"parent runtime must remain unchanged"),
			"Set parent runtime sentinel");
	profile.environment = {{"XR_RUNTIME_JSON", "ignored child override"}, {"SIMXR_EXPECTED_PRIVATE_RUNTIME", manifest}};
	const auto user = ReadRuntimeRegistration(HKEY_CURRENT_USER);
	Require(launcher.Launch(profile), "Admit explicitly provisioned private-prefix launch");
	const auto state = Terminal(launcher);
	Require(state.state == LaunchState::Exited && state.exitCode == 41 && state.process,
			"Private-prefix child must observe selected registration with no runtime environment override");
	Require(ReadRuntimeRegistration(HKEY_LOCAL_MACHINE) == ManifestRegistration(windows::Wide(manifest)),
			"Private-prefix launch registers the canonical generated manifest");
	Require(ReadRuntimeRegistration(HKEY_CURRENT_USER) == user &&
				windows::Environment("XR_RUNTIME_JSON") == "parent runtime must remain unchanged",
			"Private-prefix launch preserves user registration and parent environment");
	std::cout << "Private Proton prefix refusals and child runtime selection passed\n";
}
void DiscoveryTests()
{
	const auto endpoint =
		std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(GetCurrentProcessId());
	const auto path = LocalPipePath(endpoint);
	const windows::Handle pipe(CreateNamedPipeW(
		path.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 1024, 1024, 0, nullptr));
	Require(pipe.Get() != INVALID_HANDLE_VALUE, "Create local discovery fixture");
	Require(RuntimeEndpointExists(endpoint), "Exact local named pipe must be discoverable");
	const auto endpoints = DiscoverRuntimeEndpoints(false);
	Require(std::find(endpoints.begin(), endpoints.end(), endpoint) != endpoints.end(),
			"Runtime discovery must include actual native pipe paths");
	const windows::Handle client(CreateFileW(path.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr,
		OPEN_EXISTING, 0, nullptr));
	Require(client.Get() != INVALID_HANDLE_VALUE, "Discovery must not consume the pipe connection");
	Require(RuntimeEndpointExists(endpoint), "Busy runtime endpoints still exist");
	Require(!RuntimeEndpointExists(endpoint + ".Absent"), "Missing runtime endpoint must not exist");
}
void LaunchTests()
{
	Directory directory;
	ParentEnvironment restore(L"SIMXR_LAUNCH_TEST");
	ParentEnvironment runtimeEnvironment(L"XR_RUNTIME_JSON");
	SetEnvironmentVariableW(L"XR_RUNTIME_JSON", nullptr);
	ParentEnvironment settingsPath(L"SIMXR_COMPANION_SETTINGS");
	const auto settings = directory.path / L"config 日本語.settings";
	Require(SetEnvironmentVariableW(L"SIMXR_COMPANION_SETTINGS", settings.c_str()), "Set Unicode settings path");
	Require(CompanionSettingsPath() == settings, "Settings path preserves inherited Unicode");
	CompanionSettings saved;
	saved.endpoint = "Unicode path persistence";
	SaveSettings(settings, saved);
	Require(LoadSettings(settings) == saved, "Save settings through a Unicode destination");
	saved.scale = 1.5f;
	SaveSettings(settings, saved);
	Require(LoadSettings(settings) == saved, "Replace settings through a Unicode destination");
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
	HANDLE handle = nullptr;
	Require(OpenProcessToken(GetCurrentProcess(), TOKEN_QUERY, &handle), "Inspect launcher integrity");
	const windows::Handle token(handle);
	alignas(TOKEN_MANDATORY_LABEL) std::array<std::byte, sizeof(TOKEN_MANDATORY_LABEL) + SECURITY_MAX_SID_SIZE> buffer{};
	DWORD size = 0;
	Require(GetTokenInformation(token.Get(), TokenIntegrityLevel, buffer.data(), static_cast<DWORD>(buffer.size()), &size),
		"Read launcher integrity");
	const auto* label = reinterpret_cast<const TOKEN_MANDATORY_LABEL*>(buffer.data());
	const auto count = *GetSidSubAuthorityCount(label->Label.Sid);
	Require(count, "Launcher token has integrity authority");
	if (*GetSidSubAuthority(label->Label.Sid, count - 1) > SECURITY_MANDATORY_MEDIUM_RID)
	{
		Require(launcher.Launch(profile), "Admit elevated runtime selection for validation");
		const auto rejected = Terminal(launcher);
		Require(rejected.state == LaunchState::Failed && !rejected.process &&
			rejected.detail.find("ignores runtime overrides") != std::string::npos,
			"Reject ignored runtime override before starting the wrong application runtime");
		profile.runtimeManifest.clear();
	}
	profile.environment.emplace_back("SIMXR_EXPECTED_RUNTIME", profile.runtimeManifest);
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
	Require(windows::Environment("XR_RUNTIME_JSON") == windows::Environment("SIMXR_EXPECTED_RUNTIME"),
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
		if (argc == 2 && std::wstring_view(argv[1]) == L"--private-prefix-child")
		{
			const auto manifest = windows::Environment("SIMXR_EXPECTED_PRIVATE_RUNTIME");
			Require(!manifest.empty() && windows::Environment("XR_RUNTIME_JSON").empty(),
					"Private-prefix child must use registration without an inherited runtime override");
			Require(ReadRuntimeRegistration(HKEY_LOCAL_MACHINE) == ManifestRegistration(windows::Wide(manifest)),
					"Private-prefix child observes the selected generated runtime manifest");
			return 41;
		}
		if (argc == 2 && std::wstring_view(argv[1]) == L"--surviving-child")
		{
			Sleep(1500);
			std::ofstream(std::filesystem::current_path() / L"survived.txt") << "alive";
			return 0;
		}
		LaunchTests();
		PrivatePrefixTests();
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
