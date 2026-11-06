#include "../TestSupport.h"
#include "app/services/CompanionSettings.h"
#include "app/services/ProcessLauncher.h"
#include "app/services/ProcessSupervisor.h"
#include "app/services/RunningApps.h"
#include "app/services/Utf8Path.h"
#include "simulation/HeadsetProfiles.h"
#include <cmath>

#include <chrono>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef __linux__
#include <spawn.h>
#include <csignal>
#include <sys/wait.h>
#include <sys/stat.h>
#include <unistd.h>
#endif

namespace
{
using namespace simxr::app;
using namespace std::chrono_literals;

struct Directory
{
	std::filesystem::path path;

	Directory()
	{
#ifdef __linux__
		char pattern[] = "/tmp/simxr-companion-settings-XXXXXX";
		Require(::mkdtemp(pattern) != nullptr, "Create settings directory");
		path = pattern;
#else
		path = std::filesystem::temp_directory_path() /
			   ("simxr-settings-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));
		std::filesystem::create_directories(path);
#endif
	}

	~Directory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}
};

template<class Work>
void Reject(Work&& work)
{
	bool rejected = false;
	try
	{
		work();
	}
	catch (const std::exception&)
	{
		rejected = true;
	}
	Require(rejected, "Malformed settings must reject");
}

void Profiles()
{
	Require(simxr::HeadsetProfiles().size() == 10 && !simxr::FindHeadsetProfile("unknown"),
			"Canonical profile surface");
	const auto* quest = simxr::FindHeadsetProfile("quest3");
	Require(quest != nullptr, "Find shared Quest3 profile");
	const auto views = simxr::ProfileViews(*quest);
	Require(views.outerWidth == 2064 && views.outerHeight == 2208 && std::abs(views.ipd - 0.064f) < 0.00001f,
			"Profile recommendations preserve optics");
	Require(std::abs(views.outerFov[0].angleLeft + 0.9424778f) < 0.00001f &&
				views.outerFov[0].angleRight != -views.outerFov[0].angleLeft,
			"Preserve asymmetric eye geometry in radians");
	const auto* generic = simxr::FindHeadsetProfile("generic");
	Require(generic && generic->eye[0].angleLeft == 0 &&
				std::abs(simxr::ProfileViews(*generic).outerFov[0].angleLeft + 0.7853982f) < 0.00001f,
			"Generic preserves legacy zero-angle sentinel and native symmetric fallback");
}

void BundledRuntimeSelection()
{
	Directory directory;
	const auto first = directory.path / u8"installation 日本語";
	const auto second = directory.path / u8"moved 日本語";
	std::filesystem::create_directory(first);
	std::ofstream(first / "openxr_simulator.json") << "{}";
	LaunchProfile profile;
	profile.name = "Bundled";
	profile.useBundledRuntime = true;
	profile.privateProtonPrefix = "private-prefix";
	ValidateLaunchProfile(profile);
	const auto resolved = ResolveLaunchProfile(profile, first);
	Require(std::filesystem::path(std::u8string(resolved.runtimeManifest.begin(), resolved.runtimeManifest.end())) ==
				first / "openxr_simulator.json",
			"Bundled runtime resolves beside the application");
	Require(profile.useBundledRuntime && profile.runtimeManifest.empty(),
			"Resolving a profile does not replace its portable selection");
	std::filesystem::rename(first, second);
	const auto moved = ResolveLaunchProfile(profile, second);
	Require(std::filesystem::path(std::u8string(moved.runtimeManifest.begin(), moved.runtimeManifest.end())) ==
				second / "openxr_simulator.json",
			"Bundled runtime follows a moved installation");
	Reject([&] { ResolveLaunchProfile(profile, first); });
	Reject([&] { ResolveLaunchProfile(profile, "relative"); });
	profile.privateProtonPrefix.clear();
	profile.useBundledRuntime = false;
	Require(ResolveLaunchProfile(profile, {}) == profile, "Legacy system runtime selection stays unchanged");
	profile.runtimeManifest = "custom-runtime.json";
	Require(ResolveLaunchProfile(profile, {}) == profile, "Custom runtime selection stays unchanged");
	profile.runtimeManifest.clear();
	profile.useBundledRuntime = true;
	CompanionSettings settings;
	settings.profiles.push_back(profile);
	settings.recent.push_back({profile, 123});
	SaveSettings(directory.path / "settings", settings);
	Require(LoadSettings(directory.path / "settings") == settings, "Portable runtime selection survives settings");
	const auto settingsPath = directory.path / "settings";
	const auto fullSize = std::filesystem::file_size(settingsPath);
	const auto extensionSize = 4 * (3 + settings.profiles.size() + settings.recent.size());
	std::filesystem::resize_file(settingsPath, fullSize - extensionSize);
	settings.profiles.front().useBundledRuntime = false;
	settings.recent.front().launch.useBundledRuntime = false;
	Require(LoadSettings(settingsPath) == settings, "Older settings retain their original runtime selection");
	profile.runtimeManifest = "simxr:bundled";
	Reject([&] { ResolveLaunchProfile(profile, second); });
	profile.useBundledRuntime = false;
	settings.profiles = {profile};
	settings.recent.clear();
	SaveSettings(directory.path / "settings", settings);
	Require(LoadSettings(directory.path / "settings") == settings && ResolveLaunchProfile(profile, {}) == profile,
			"Legacy manifest text is never reinterpreted as a bundled runtime");
}

void SteamRuntimeOption()
{
	Directory directory;
	const auto manifest = directory.path / u8"runtime ' 日本語 $(printf injected).json";
	auto option = LinuxSteamRuntimeOption(manifest);
	const auto marker = option.find("%command%");
	Require(marker != std::string::npos, "Steam option retains the game command placeholder");
#ifdef __linux__
	option.replace(marker, std::string_view("%command%").size(), "/bin/sh -c 'printf \"%s\" \"$XR_RUNTIME_JSON\"'");
	const auto output = directory.path / "actual";
	Require(std::system((option + " > " + output.string()).c_str()) == 0, "Steam option shell fixture failed");
	std::ifstream input(output, std::ios::binary);
	const std::string actual((std::istreambuf_iterator<char>(input)), {});
	Require(actual == PathText(manifest), "Steam option did not preserve the literal Unicode manifest path");
#endif
	Reject([&] { LinuxSteamRuntimeOption("relative.json"); });
	Reject([&] { LinuxSteamRuntimeOption(directory.path / "%command%.json"); });
}

void ProfileCommand()
{
	Directory directory;
	const auto executable = directory.path / u8"program ' 日本語 $(touch injected)";
	const std::string name = "--game '\\ \" 日本語 $(touch injected)";
	const auto command = ProfileLaunchCommand(executable, name, LaunchCommandShell::Posix);
#ifdef __linux__
	{
		std::ofstream script(executable);
		script << "#!/bin/sh\nprintf '%s' \"$1\" > \"$SIMXR_COMMAND_RESULT\"\n";
	}
	Require(::chmod(executable.c_str(), 0700) == 0, "Make command fixture executable");
	const auto result = directory.path / "result";
	Require(::setenv("SIMXR_COMMAND_RESULT", result.c_str(), 1) == 0, "Set command fixture result");
	Require(std::system(command.c_str()) == 0, "Run quoted launch command");
	::unsetenv("SIMXR_COMMAND_RESULT");
	std::ifstream input(result, std::ios::binary);
	Require(std::string(std::istreambuf_iterator<char>(input), {}) == "--launch-profile=" + name,
			"Launch command must pass the profile name literally");
#endif
	const auto powershell = ProfileLaunchCommand(executable, name, LaunchCommandShell::PowerShell);
	Require(powershell.starts_with("& '") && powershell.find("''") != std::string::npos,
			"PowerShell command quotes executable and profile");
	const auto encoded = powershell.find("--launch-profile-hex=");
	Require(encoded != std::string::npos && powershell.back() == '\'', "PowerShell uses a safe ASCII profile argument");
	const auto begin = encoded + std::string_view("--launch-profile-hex=").size();
	Require(DecodeProfileLaunchName(std::string_view(powershell).substr(begin, powershell.size() - begin - 1)) == name,
			"PowerShell profile argument preserves quotes, backslashes and Unicode");
	Reject([&] { DecodeProfileLaunchName("abc"); });
	Reject([&] { DecodeProfileLaunchName("gh"); });
	Reject([&] { DecodeProfileLaunchName("00"); });
	Reject([&] { ProfileLaunchCommand("relative", name, LaunchCommandShell::Posix); });
	Reject([&] { ProfileLaunchCommand(executable, "", LaunchCommandShell::Posix); });
	Reject([&] { ProfileLaunchCommand(executable, std::string("a\0b", 3), LaunchCommandShell::Posix); });
}

void McpPreference()
{
	Directory directory;
	const auto file = directory.path / "settings";
	CompanionSettings settings;
	Require(!LoadSettings(file).mcpEnabled, "MCP defaults to disabled");
	settings.mcpEnabled = true;
	SaveSettings(file, settings);
	Require(LoadSettings(file) == settings, "MCP preference survives restart");
	const auto size = std::filesystem::file_size(file);
	std::filesystem::resize_file(file, size - 8);
	settings.mcpEnabled = false;
	Require(LoadSettings(file) == settings, "Earlier settings default MCP to disabled");
	SaveSettings(file, settings);
	Require(LoadSettings(file) == settings, "Disabled MCP preference is retained");
}

void UpdatePreference()
{
	Directory directory;
	const auto file = directory.path / "settings";
	CompanionSettings settings;
	Require(!LoadSettings(file).autoUpdateCheck, "Automatic update checks default to off");
	settings.autoUpdateCheck = true;
	SaveSettings(file, settings);
	Require(LoadSettings(file) == settings, "Update preference survives restart");
	std::filesystem::resize_file(file, std::filesystem::file_size(file) - 8);
	settings.autoUpdateCheck = false;
	Require(LoadSettings(file) == settings, "Earlier settings default automatic updates to off");
}

void SessionPreference()
{
	Directory directory;
	const auto file = directory.path / "settings";
	CompanionSettings settings;
	Require(LoadSettings(file).openNewSessions, "New apps open in tabs by default");
	settings.openNewSessions = false;
	SaveSettings(file, settings);
	Require(LoadSettings(file) == settings, "Disabled automatic tabs survive restart");
	std::filesystem::resize_file(file, std::filesystem::file_size(file) - 8);
	settings.openNewSessions = true;
	Require(LoadSettings(file) == settings, "Earlier settings open new apps in tabs");
}

void DeviceKeys()
{
	Directory directory;
	const auto file = directory.path / "settings";
	const auto key = [](CompanionSettings& settings, PilotAction action) -> PilotBinding& {
		return settings.pilotBindings[static_cast<std::size_t>(action)];
	};
	CompanionSettings settings;
	settings.mcpEnabled = true;
	key(settings, PilotAction::TargetPlayer) = {"0"};
	key(settings, PilotAction::TargetHead) = {"1"};
	key(settings, PilotAction::TargetLeft) = {"2"};
	key(settings, PilotAction::TargetRight) = {"3"};
	SaveSettings(file, settings);
	auto loaded = LoadSettings(file);
	Require(key(loaded, PilotAction::TargetPlayer) == PilotBinding{"1"} &&
				key(loaded, PilotAction::TargetRight) == PilotBinding{"4"} &&
				key(loaded, PilotAction::TargetTrackers) == PilotBinding{"7"},
			"Settings from before device keys started at 1 keep the old defaults");
	key(settings, PilotAction::TargetHead) = {"H"};
	key(settings, PilotAction::TargetGaze) = {"G", 1};
	SaveSettings(file, settings);
	Require(LoadSettings(file) == settings, "Device keys survive restart without renumbering custom bindings");
}

void ConcurrentSettings()
{
	Directory directory;
	const auto file = directory.path / "settings";
	CompanionSettings baseline;
	SaveSettings(file, baseline);
	auto first = baseline, second = baseline;
	first.profiles.push_back({"First"});
	second.profiles.push_back({"Second"});
	std::atomic<unsigned> saved = 0, rejected = 0;
	const auto write = [&](const CompanionSettings& candidate) {
		try
		{
			SaveSettingsIfUnchanged(file, baseline, candidate);
			++saved;
		}
		catch (const std::exception&)
		{
			++rejected;
		}
	};
	std::thread a(write, std::cref(first)), b(write, std::cref(second));
	a.join();
	b.join();
	Require(saved == 1 && rejected == 1, "Concurrent stale settings must not overwrite each other");
	const auto actual = LoadSettings(file);
	Require(actual == first || actual == second, "Concurrent settings retained neither complete update");
	Reject([&] { SaveSettingsIfUnchanged(file, baseline, baseline); });
	Require(LoadSettings(file) == actual, "Rejected stale settings must preserve the newer profile");
}

void Persistence()
{
	Directory directory;
	const auto file = directory.path / u8"nested 日本語/companion.settings";
	Require(LoadSettings(file) == CompanionSettings{}, "Missing config has defaults");
	const auto layoutFile = directory.path / u8"window 配置/詳細.ini";
	const std::string layout = "[Window][詳細##日本語]\r\nPos=12,34\r\nSize=560,320\r\n";
	Require(LoadWindowLayout(layoutFile).empty(), "Missing window layout is empty");
	SaveWindowLayout(layoutFile, layout);
	Require(LoadWindowLayout(layoutFile) == layout, "Window layout preserves Unicode paths and exact text");
	Reject([&] { SaveWindowLayout(layoutFile, std::string(8 * 1024 * 1024 + 1, 'x')); });
	Reject([&] { SaveWindowLayout(layoutFile, std::string("before\0after", 12)); });
	Require(LoadWindowLayout(layoutFile) == layout, "Rejected window layout save preserves the original");
	SaveWindowLayout(layoutFile, {});
	Require(LoadWindowLayout(layoutFile).empty(), "Empty window layout replaces previous text");
	SaveWindowLayout(layoutFile, layout);
	std::filesystem::resize_file(layoutFile, 8 * 1024 * 1024 + 1);
	Reject([&] { LoadWindowLayout(layoutFile); });
	CompanionSettings settings;
	settings.scale = 1.375f;
	settings.snapshotBudgetMiB = 2048;
	settings.previewBudgetMiB = 4096;
	settings.previewRamMiB = 2048;
	settings.pilotBindings[static_cast<std::size_t>(PilotAction::Forward)] = {"UpArrow", 2};
	settings.gamepadBindings.primary[1] = "x";
	settings.gamepadBindings.gyroHold = "paddle1";
	settings.gamepadBindings.deadzone = .2f;
	settings.endpoint = "/tmp/control.9.sock";
	settings.activeProfile = "A profile";
	settings.activeLayout = "Wide";
	settings.profiles.push_back({"A profile",
								 "/usr/bin/wine",
								 {"game with spaces.exe", "", "a\nb", "$literal; no shell"},
								 "/games",
								 "/runtime.json",
								 {{"WINEPREFIX", "/prefix"}, {"VALUE", "a=b\nc"}},
								 "quest3",
								 "rig.lua",
								 "Z:\\隔離\\named prefix\\pfx"});
	auto secondProfile = settings.profiles.front();
	secondProfile.name = "Other profile";
	secondProfile.privateProtonPrefix = "Z:\\別の prefix\\pfx";
	settings.profiles.push_back(std::move(secondProfile));
	settings.layouts.push_back({"Wide", "[Window][GPU preview]\nPos=1,2\nSize=300,200\n"});
	for (std::int64_t index = 0; index < 10; ++index)
		RememberRecentApp(settings, {"App", "/games/app" + std::to_string(index % 9), {"--level", "1"}, "/games"},
						  1'700'000'000'000 + index);
	Require(settings.recent.size() == MaxRecentApps && settings.recent.front().lastUsed == 1'700'000'000'009 &&
				settings.recent.front().launch.executable == "/games/app0",
			"Recent apps keep the newest distinct entries first");
	settings.recent.front().launch.runtimeManifest = "Z:\\runtime 日本語\\openxr_simulator.json";
	settings.recent.front().launch.privateProtonPrefix = "Z:\\最近\\first\\pfx";
	settings.recent.back().launch.runtimeManifest = "Z:\\other runtime\\openxr_simulator.json";
	settings.recent.back().launch.privateProtonPrefix = "Z:\\最近\\last\\pfx";
	settings.remoteMouseDrag = true;
	SaveSettings(file, settings);
	for (int index = 0; index < 80; ++index)
		settings.recent.front().launch.environment.emplace_back("CUSTOM_" + std::to_string(index),
																index % 2 ? "" : "literal value ; $()");
	SaveSettings(file, settings);
	Require(LoadSettings(file) == settings, "Every profile/layout/recent field round trips exactly");
	{
		std::ifstream input(file, std::ios::binary);
		const std::string complete((std::istreambuf_iterator<char>(input)), {});
		Require(!input.bad(), "Read serialized settings fixture");
		input.close();
		Require(!input.fail(), "Close serialized settings fixture");
		std::string extension;
		const auto number = [&](std::uint32_t value) {
			for (unsigned shift = 0; shift < 32; shift += 8)
				extension.push_back(static_cast<char>(value >> shift));
		};
		const auto prefix = [&](const std::string& value) {
			number(static_cast<std::uint32_t>(value.size()));
			extension += value;
		};
		number(7);
		number(static_cast<std::uint32_t>(settings.profiles.size()));
		for (const auto& profile : settings.profiles)
			prefix(profile.privateProtonPrefix);
		const auto recentCount = extension.size();
		number(static_cast<std::uint32_t>(settings.recent.size()));
		for (const auto& recent : settings.recent)
			prefix(recent.launch.privateProtonPrefix);
		number(8);
		number(1);
		number(9);
		number(settings.previewBudgetMiB);
		number(10);
		number(settings.previewRamMiB);
		Require(complete.ends_with(extension), "Private prefix and pointer settings use versioned trailing tables");
		const auto extensionStart = complete.size() - extension.size();
		const auto replace = [&](std::string_view bytes) {
			std::ofstream output(file, std::ios::binary | std::ios::trunc);
			output.write(bytes.data(), static_cast<std::streamsize>(bytes.size()));
			output.close();
			Require(output.good(), "Write settings migration fixture");
		};
		auto previous = settings;
		previous.previewRamMiB = simxr::FramePreviewHistory::RamBudgetBytes >> 20;
		replace(std::string_view(complete).substr(0, complete.size() - 8));
		Require(LoadSettings(file) == previous, "Version-9 settings receive default RAM history budget");
		previous.previewBudgetMiB = simxr::FramePreviewHistory::BudgetBytes >> 20;
		replace(std::string_view(complete).substr(0, complete.size() - 16));
		Require(LoadSettings(file) == previous, "Version-8 settings receive default history budget");
		previous.remoteMouseDrag = false;
		replace(std::string_view(complete).substr(0, complete.size() - 24));
		Require(LoadSettings(file) == previous, "Version-7 settings default to relative mouse capture");
		replace(std::string_view(complete).substr(0, extensionStart));
		for (auto& profile : previous.profiles)
			profile.privateProtonPrefix.clear();
		for (auto& recent : previous.recent)
			recent.launch.privateProtonPrefix.clear();
		Require(LoadSettings(file) == previous,
				"Version-6 settings retain data and default every private prefix to empty");
		for (const auto offset : {std::size_t{4}, recentCount})
		{
			auto corrupt = complete;
			corrupt[extensionStart + offset] = 0;
			replace(corrupt);
			Reject([&] { LoadSettings(file); });
		}
		replace(std::string_view(complete).substr(0, extensionStart + 6));
		Reject([&] { LoadSettings(file); });
		replace(std::string_view(complete).substr(0, complete.size() - 1));
		Reject([&] { LoadSettings(file); });
		replace(complete);
	}
	{
		auto precise = settings;
		precise.gamepadBindings.deadzone = .9999f;
		precise.crops.push_back({"default", .999998f, .500001f, 1.0f, .500002f});
		SaveSettings(file, precise);
		const auto restored = LoadSettings(file);
		Require(restored.gamepadBindings.deadzone < 1 && restored.crops.back().x0 < restored.crops.back().x1 &&
					restored.crops.back().y0 < restored.crops.back().y1,
				"Fixed-point settings preserve valid deadzones and nonempty crops");
		SaveSettings(file, settings);
	}
#ifdef __linux__
	Require(PathProblem("/bin/sh", PathKind::Executable).empty() &&
				PathProblem("/nonexistent/simxr-app", PathKind::Executable) == "File not found" &&
				PathProblem("/", PathKind::Executable) == "This is a folder" &&
				PathProblem(file.string(), PathKind::Executable) == "Not executable" &&
				PathProblem(file.string(), PathKind::Folder) == "Not a folder" &&
				PathProblem("", PathKind::File) == "Required",
			"Launch paths are validated with a short reason");
#endif
#ifdef __linux__
	struct stat info{};
	Require(::stat(file.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "Settings file remains private");
#endif
	auto invalid = settings;
	invalid.profiles.front().runtimeManifest.clear();
	Reject([&] { SaveSettings(file, invalid); });
	invalid = settings;
	invalid.recent.front().launch.runtimeManifest.clear();
	Reject([&] { SaveSettings(file, invalid); });
	Require(LoadSettings(file) == settings,
			"Private prefixes without a runtime manifest cannot replace saved settings");
	invalid = settings;
	invalid.profiles.push_back(settings.profiles.front());
	Reject([&] { SaveSettings(file, invalid); });
	Require(LoadSettings(file) == settings, "Rejected save preserves original file");
	invalid = settings;
	invalid.scale = 0;
	Reject([&] { SaveSettings(file, invalid); });
	invalid = settings;
	invalid.layouts.front().ini = std::string(1024 * 1024 + 1, 'x');
	Reject([&] { SaveSettings(file, invalid); });
	settings.scale = 2;
	SaveSettings(file, settings);
	Require(LoadSettings(file) == settings, "Atomic replacement updates previous settings");
	{
		Directory failed;
		const auto destination = failed.path / u8"保存.settings";
		std::filesystem::create_directory(destination);
		Reject([&] { SaveSettings(destination, settings); });
		Reject([&] { SaveWindowLayout(destination, layout); });
		Reject([&] { LoadWindowLayout(destination); });
		Require(std::filesystem::is_directory(destination) && std::filesystem::is_empty(destination),
				"Failed replacement preserves the destination");
		auto lockPath = destination;
		lockPath += ".lock";
		std::size_t entries = 0;
		for (const auto& entry : std::filesystem::directory_iterator(failed.path))
		{
			Require(entry.path() == destination || entry.path() == lockPath,
					"Failed replacement removes its temporary file");
			++entries;
		}
		Require(entries == 2, "Failed replacement preserves the destination and settings lock");
	}
	std::ofstream(file, std::ios::binary | std::ios::app).put('x');
	Reject([&] { LoadSettings(file); });
	std::ofstream(file, std::ios::binary | std::ios::trunc) << "SIMXR-COMPANION\x02";
	Reject([&] { LoadSettings(file); });
	SaveSettings(file, settings);
	std::filesystem::resize_file(file, 20);
	Reject([&] { LoadSettings(file); });
}
#ifdef __linux__
void Launch(const std::filesystem::path& executable)
{
	Directory directory;
	std::ofstream(directory.path / "runtime.json") << "{}";
	ProcessSupervisor processes;
	ProcessLauncher launcher(processes);
	LaunchProfile profile;
	profile.name = "Literal child";
	profile.executable = executable.string();
	profile.arguments = {"--child", "literal $(never-execute) ; spaces"};
	profile.workingDirectory = directory.path.string();
	profile.runtimeManifest = (directory.path / "runtime.json").string();
	profile.environment = {{"SIMXR_LAUNCH_TEST", "only child"}};
	profile.headsetProfile = "quest3";
	const std::string before = std::getenv("XR_RUNTIME_JSON") ? std::getenv("XR_RUNTIME_JSON") : "";
	Require(launcher.Launch(profile), "Admit launch");
	const auto deadline = std::chrono::steady_clock::now() + 3s;
	LaunchSnapshot state;
	do
	{
		state = launcher.Snapshot();
		if (state.state == LaunchState::Exited || state.state == LaunchState::Failed)
			break;
		std::this_thread::sleep_for(5ms);
	}
	while (std::chrono::steady_clock::now() < deadline);
	Require(state.state == LaunchState::Exited && state.exitCode == 0 && state.process != 0,
			"Observe actual child exit");
	std::ifstream result(directory.path / "result.txt");
	std::string value((std::istreambuf_iterator<char>(result)), {});
	Require(value == profile.runtimeManifest + "\nonly child\nquest3\nliteral $(never-execute) ; spaces",
			"Child cwd, manifest, environment and literal argv applied");
	Require((std::getenv("XR_RUNTIME_JSON") ? std::getenv("XR_RUNTIME_JSON") : "") == before,
			"Parent runtime selection unchanged");
	profile.privateProtonPrefix = (directory.path / "pfx").string();
	Require(launcher.Launch(profile), "Admit unsupported private-prefix launch for validation");
	Eventually(
		[&] {
			state = launcher.Snapshot();
			return state.state == LaunchState::Failed || state.state == LaunchState::Exited;
		},
		"Unsupported private-prefix launch did not settle");
	Require(state.state == LaunchState::Failed && !state.process && state.detail.find("Proton") != std::string::npos,
			"Linux private-prefix launch must reject before spawning a child");
	profile.privateProtonPrefix.clear();
	const auto validManifest = profile.runtimeManifest;
	profile.runtimeManifest = (directory.path / "missing-runtime.json").string();
	Require(launcher.Launch(profile), "Admit missing runtime launch");
	Eventually(
		[&] {
			state = launcher.Snapshot();
			return state.state == LaunchState::Failed || state.state == LaunchState::Exited;
		},
		"Missing runtime launch did not settle");
	Require(state.state == LaunchState::Failed && state.detail.find("Runtime manifest") != std::string::npos,
			"Missing runtime must fail before starting the application");
	profile.runtimeManifest = validManifest;
	profile.executable = "/no/such/executable";
	Require(launcher.Launch(profile), "Admit failed launch");
	do
	{
		state = launcher.Snapshot();
		if (state.state == LaunchState::Failed)
			break;
		std::this_thread::sleep_for(5ms);
	}
	while (std::chrono::steady_clock::now() < deadline);
	Require(state.state == LaunchState::Failed && state.detail.find("Launch failed") != std::string::npos,
			"Missing executable has actionable error");
	profile.executable = executable.string();
	profile.workingDirectory = "/no/such/directory";
	Require(launcher.Launch(profile), "Admit bad cwd launch");
	do
	{
		state = launcher.Snapshot();
		if (state.state == LaunchState::Failed)
			break;
		std::this_thread::sleep_for(5ms);
	}
	while (std::chrono::steady_clock::now() < deadline);
	Require(state.state == LaunchState::Failed, "Invalid cwd fails explicitly");
}

void ProcessDescriptions(const std::filesystem::path& executable)
{
	for (const bool oversized : {false, true})
	{
		std::vector<std::string> values{executable.string(), "--description-child", "", "literal 日本語 ; $()"};
		if (oversized)
			values.resize(258, "must not be truncated");
		std::vector<char*> arguments;
		for (auto& value : values)
			arguments.push_back(value.data());
		arguments.push_back(nullptr);
		std::vector<std::string> environment;
		for (auto value = environ; value && *value; ++value)
			if (!std::string_view(*value).starts_with("XR_RUNTIME_JSON="))
				environment.emplace_back(*value);
		environment.emplace_back("XR_RUNTIME_JSON=");
		std::vector<char*> variables;
		for (auto& value : environment)
			variables.push_back(value.data());
		variables.push_back(nullptr);
		pid_t process = 0;
		Require(::posix_spawn(&process, executable.c_str(), nullptr, nullptr, arguments.data(), variables.data()) == 0,
				"Start process description fixture");

		struct StopChild
		{
			pid_t process;

			~StopChild()
			{
				::kill(process, SIGKILL);
				while (::waitpid(process, nullptr, 0) < 0 && errno == EINTR)
				{}
			}
		} child{process};

		int status = 0;
		pid_t waited;
		do
		{
			waited = ::waitpid(process, &status, WUNTRACED);
		}
		while (waited < 0 && errno == EINTR);
		Require(waited == process && WIFSTOPPED(status), "Wait for process description fixture startup");
		Require(::kill(process, SIGCONT) == 0, "Resume process description fixture");
		const auto described = DescribeProcess(static_cast<std::uint64_t>(process));
		if (oversized)
			Require(!described, "An oversized process description must not silently truncate its arguments");
		else
		{
			Require(described.has_value(), "Process description is unavailable");
			Require(described->executable == executable.string(), "Process description changed its executable");
			Require(described->arguments == std::vector<std::string>(values.begin() + 1, values.end()),
					"Process description changed empty/Unicode/literal arguments");
			Require(described->workingDirectory == std::filesystem::current_path().string(),
					"Process description changed its working directory");
			Require(described->runtimeManifest.empty() &&
						std::find(described->environment.begin(), described->environment.end(),
								  std::pair<std::string, std::string>{"XR_RUNTIME_JSON", ""}) !=
							described->environment.end(),
					"Process description dropped an empty runtime override");
		}
	}
}
#endif
} // namespace

int main(int argc, char** argv)
{
	if (argc == 6 && std::string_view(argv[1]) == "--settings-race-fixture")
	{
		try
		{
			const auto baseline = LoadSettings(argv[2]);
			auto candidate = baseline;
			candidate.profiles.push_back({argv[5]});
			std::ofstream(argv[3]).put('1');
			const auto deadline = std::chrono::steady_clock::now() + 10s;
			while (!std::filesystem::exists(argv[4]) && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(5ms);
			if (!std::filesystem::exists(argv[4]))
				return 4;
			SaveSettingsIfUnchanged(argv[2], baseline, candidate);
			return 0;
		}
		catch (const std::exception& error)
		{
			std::cerr << error.what() << '\n';
			return 3;
		}
	}
	if (argc == 5 && std::string_view(argv[1]) == "--profile-fixture")
	{
		try
		{
			CompanionSettings settings;
			LaunchProfile profile;
			profile.name = "--game '\\ \" 日本語 $(touch injected)";
			profile.executable = PathText(std::filesystem::absolute(argv[0]));
			profile.arguments = {"--child", "argument with spaces"};
			profile.workingDirectory = argv[3];
			profile.runtimeManifest = PathText(std::filesystem::path(argv[3]) / "runtime.json");
			profile.environment = {{"SIMXR_LAUNCH_TEST", "literal value"}};
			profile.headsetProfile = "generic";
			settings.profiles.push_back(profile);
			SaveSettings(argv[2], settings);
			std::cout << ProfileLaunchCommand(std::filesystem::absolute(argv[4]), profile.name,
											  LaunchCommandShell::Posix);
			return 0;
		}
		catch (const std::exception& error)
		{
			std::cerr << error.what() << '\n';
			return 1;
		}
	}
	if (argc >= 2 && std::string_view(argv[1]) == "--description-child")
	{
#ifdef __linux__
		::raise(SIGSTOP);
#endif
		std::this_thread::sleep_for(10s);
		return 0;
	}
	if (argc == 3 && std::string_view(argv[1]) == "--child")
	{
		const auto manifest = std::getenv("XR_RUNTIME_JSON"), value = std::getenv("SIMXR_LAUNCH_TEST");
		const auto headset = std::getenv("SIMXR_HEADSET_PROFILE");
		if (!manifest || !value || !headset)
			return 9;
		std::ofstream("result.txt") << manifest << '\n' << value << '\n' << headset << '\n' << argv[2];
		return 0;
	}
	try
	{
		Profiles();
		BundledRuntimeSelection();
		SteamRuntimeOption();
		ProfileCommand();
		McpPreference();
		UpdatePreference();
		SessionPreference();
		DeviceKeys();
		ConcurrentSettings();
		Persistence();
#ifdef __linux__
		Launch(std::filesystem::absolute(argv[0]));
		ProcessDescriptions(std::filesystem::absolute(argv[0]));
#endif
		std::cout << "PASS companion settings, layouts and child process launch\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
