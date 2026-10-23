#include "../TestSupport.h"
#include "app/services/CompanionSettings.h"
#include "app/services/ProcessLauncher.h"
#include "app/services/ProcessSupervisor.h"
#include "app/services/RunningApps.h"
#include "simulation/HeadsetProfiles.h"
#include <cmath>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef __linux__
#include <spawn.h>
#include <signal.h>
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
template <class Work> void Reject(Work&& work)
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
	Reject([&]
	{
		SaveWindowLayout(layoutFile, std::string(8 * 1024 * 1024 + 1, 'x'));
	});
	Reject([&]
	{
		SaveWindowLayout(layoutFile, std::string("before\0after", 12));
	});
	Require(LoadWindowLayout(layoutFile) == layout, "Rejected window layout save preserves the original");
	SaveWindowLayout(layoutFile, {});
	Require(LoadWindowLayout(layoutFile).empty(), "Empty window layout replaces previous text");
	SaveWindowLayout(layoutFile, layout);
	std::filesystem::resize_file(layoutFile, 8 * 1024 * 1024 + 1);
	Reject([&]
	{
		LoadWindowLayout(layoutFile);
	});
	CompanionSettings settings;
	settings.scale = 1.375f;
	settings.snapshotBudgetMiB = 2048;
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
								 "rig.lua"});
	settings.layouts.push_back({"Wide", "[Window][GPU preview]\nPos=1,2\nSize=300,200\n"});
	for (std::int64_t index = 0; index < 10; ++index)
		RememberRecentApp(settings, {"App", "/games/app" + std::to_string(index % 9), {"--level", "1"}, "/games"},
						  1'700'000'000'000 + index);
	Require(settings.recent.size() == MaxRecentApps && settings.recent.front().lastUsed == 1'700'000'000'009 &&
				settings.recent.front().launch.executable == "/games/app0",
			"Recent apps keep the newest distinct entries first");
	SaveSettings(file, settings);
	for (int index = 0; index < 80; ++index)
		settings.recent.front().launch.environment.emplace_back("CUSTOM_" + std::to_string(index),
			index % 2 ? "" : "literal value ; $()");
	SaveSettings(file, settings);
	Require(LoadSettings(file) == settings, "Every profile/layout/recent field round trips exactly");
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
				PathProblem(file.string(), PathKind::Folder) == "Not a folder" && PathProblem("", PathKind::File) == "Required",
			"Launch paths are validated with a short reason");
#endif
#ifdef __linux__
	struct stat info{};
	Require(::stat(file.c_str(), &info) == 0 && (info.st_mode & 0777) == 0600, "Settings file remains private");
#endif
	auto invalid = settings;
	invalid.profiles.push_back(settings.profiles.front());
	Reject(
		[&]
		{
			SaveSettings(file, invalid);
		});
	Require(LoadSettings(file) == settings, "Rejected save preserves original file");
	invalid = settings;
	invalid.scale = 0;
	Reject(
		[&]
		{
			SaveSettings(file, invalid);
		});
	invalid = settings;
	invalid.layouts.front().ini = std::string(1024 * 1024 + 1, 'x');
	Reject(
		[&]
		{
			SaveSettings(file, invalid);
		});
	settings.scale = 2;
	SaveSettings(file, settings);
	Require(LoadSettings(file) == settings, "Atomic replacement updates previous settings");
	{
		Directory failed;
		const auto destination = failed.path / u8"保存.settings";
		std::filesystem::create_directory(destination);
		Reject(
			[&]
			{
				SaveSettings(destination, settings);
			});
		Reject([&]
		{
			SaveWindowLayout(destination, layout);
		});
		Reject([&]
		{
			LoadWindowLayout(destination);
		});
		Require(std::filesystem::is_directory(destination) && std::filesystem::is_empty(destination),
			"Failed replacement preserves the destination");
		std::size_t entries = 0;
		for (const auto& entry : std::filesystem::directory_iterator(failed.path))
		{
			Require(entry.path() == destination, "Failed replacement removes its temporary file");
			++entries;
		}
		Require(entries == 1, "Failed replacement preserves only its destination");
	}
	std::ofstream(file, std::ios::binary | std::ios::app).put('x');
	Reject(
		[&]
		{
			LoadSettings(file);
		});
	std::ofstream(file, std::ios::binary | std::ios::trunc) << "SIMXR-COMPANION\x02";
	Reject(
		[&]
		{
			LoadSettings(file);
		});
	SaveSettings(file, settings);
	std::filesystem::resize_file(file, 20);
	Reject(
		[&]
		{
			LoadSettings(file);
		});
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
	} while (std::chrono::steady_clock::now() < deadline);
	Require(state.state == LaunchState::Exited && state.exitCode == 0 && state.process != 0,
			"Observe actual child exit");
	std::ifstream result(directory.path / "result.txt");
	std::string value((std::istreambuf_iterator<char>(result)), {});
	Require(value == profile.runtimeManifest + "\nonly child\nquest3\nliteral $(never-execute) ; spaces",
			"Child cwd, manifest, environment and literal argv applied");
	Require((std::getenv("XR_RUNTIME_JSON") ? std::getenv("XR_RUNTIME_JSON") : "") == before,
			"Parent runtime selection unchanged");
	const auto validManifest = profile.runtimeManifest;
	profile.runtimeManifest = (directory.path / "missing-runtime.json").string();
	Require(launcher.Launch(profile), "Admit missing runtime launch");
	Eventually(
		[&]
		{
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
	} while (std::chrono::steady_clock::now() < deadline);
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
	} while (std::chrono::steady_clock::now() < deadline);
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
				{
				}
			}
		} child{process};
		int status = 0;
		pid_t waited;
		do
		{
			waited = ::waitpid(process, &status, WUNTRACED);
		} while (waited < 0 && errno == EINTR);
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
			Require(described->runtimeManifest.empty() && std::find(described->environment.begin(),
				described->environment.end(), std::pair<std::string, std::string>{"XR_RUNTIME_JSON", ""}) !=
				described->environment.end(), "Process description dropped an empty runtime override");
		}
	}
}
#endif
} // namespace
int main(int argc, char** argv)
{
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
