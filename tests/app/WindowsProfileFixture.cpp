#include "Pch.h"
#include "app/platform/windows/LaunchSupport.h"
#include "app/services/CompanionSettings.h"
#include "app/services/LaunchTypes.h"
#include "app/services/Utf8Path.h"

#include <shellapi.h>
#include <tlhelp32.h>
#include <fcntl.h>
#include <io.h>
#include <iostream>

using namespace simxr::app;

static std::filesystem::path OwnPath()
{
	std::array<wchar_t, 32768> buffer{};
	const auto count = GetModuleFileNameW(nullptr, buffer.data(), static_cast<DWORD>(buffer.size()));
	if (!count || count >= buffer.size())
		throw windows::Error("Cannot find profile fixture");
	return std::filesystem::path(std::wstring_view(buffer.data(), count));
}

static DWORD ParentProcess()
{
	windows::Handle snapshot(CreateToolhelp32Snapshot(TH32CS_SNAPPROCESS, 0));
	if (snapshot.Get() == INVALID_HANDLE_VALUE)
		throw windows::Error("Cannot inspect profile fixture parent");
	PROCESSENTRY32W entry{};
	entry.dwSize = sizeof(entry);
	for (bool found = Process32FirstW(snapshot.Get(), &entry) != FALSE; found;
		 found = Process32NextW(snapshot.Get(), &entry) != FALSE)
		if (entry.th32ProcessID == GetCurrentProcessId())
			return entry.th32ParentProcessID;
	throw std::runtime_error("Cannot find profile fixture parent");
}

static int Setup()
{
	const auto root = Utf8Path(windows::Environment("SIMXR_PROFILE_FIXTURE_ROOT"));
	const auto application = Utf8Path(windows::Environment("SIMXR_PROFILE_FIXTURE_APP"));
	const auto settingsPath = Utf8Path(windows::Environment("SIMXR_COMPANION_SETTINGS"));
	if (!root.is_absolute() || !application.is_absolute() || !settingsPath.is_absolute())
		throw std::runtime_error("Profile fixture paths must be absolute");
	LaunchProfile profile;
	profile.name = "--profile ' 日本語 $(Set-Content -Path injected.txt -Value injected)";
	profile.executable = PathText(OwnPath());
	profile.workingDirectory = PathText(root / L"working Ω '$");
	profile.arguments = {"--child", "", "argument with spaces", "日本語 '$(ignored)"};
	profile.environment = {{"SIMXR_PROFILE_SENTINEL", "literal Ω $ value"}};
	profile.headsetProfile = "generic";
	profile.useBundledRuntime = true;
	CompanionSettings settings;
	settings.profiles.push_back(profile);
	SaveSettings(settingsPath, settings);
	const auto command = ProfileLaunchCommand(application, profile.name, LaunchCommandShell::PowerShell);
	_setmode(_fileno(stdout), _O_BINARY);
	std::cout.write(command.data(), static_cast<std::streamsize>(command.size()));
	return std::cout ? 0 : 1;
}

static int Child()
{
	int count = 0;
	const auto raw = CommandLineToArgvW(GetCommandLineW(), &count);
	if (!raw)
		throw windows::Error("Cannot inspect profile fixture arguments");
	const std::unique_ptr<wchar_t*, decltype(&LocalFree)> arguments(raw, LocalFree);
	const auto root = Utf8Path(windows::Environment("SIMXR_PROFILE_FIXTURE_ROOT"));
	std::ofstream output(root / "result.txt", std::ios::binary);
	output << ParentProcess() << '\n'
		   << GetCurrentProcessId() << '\n'
		   << PathText(std::filesystem::current_path()) << '\n'
		   << windows::Environment("SIMXR_PROFILE_SENTINEL") << '\n'
		   << windows::Environment("SIMXR_HEADSET_PROFILE") << '\n'
		   << windows::Environment("XR_RUNTIME_JSON") << '\n'
		   << count << '\n';
	for (int index = 1; index < count; ++index)
		output << windows::Utf8(arguments.get()[index]) << '\n';
	output << "done\n";
	output.close();
	if (!output)
		throw std::runtime_error("Cannot write profile fixture result");
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(30);
	const auto release = root / "release-child";
	while (!std::filesystem::exists(release) && std::chrono::steady_clock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(10));
	return std::filesystem::exists(release) ? 0 : 3;
}

int main(int argc, char** argv)
{
	try
	{
		if (argc == 2 && std::string_view(argv[1]) == "--setup")
			return Setup();
		if (argc >= 2 && std::string_view(argv[1]) == "--child")
			return Child();
		return 2;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
