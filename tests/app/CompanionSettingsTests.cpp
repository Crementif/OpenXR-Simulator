#include "../TestSupport.h"
#include "app/CompanionSettings.h"
#include "app/ProcessLauncher.h"
#include "app/ProcessSupervisor.h"
#include "simulation/HeadsetProfiles.h"
#include <cmath>

#include <chrono>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <thread>
#ifdef __linux__
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
	const auto file = directory.path / "nested/companion.settings";
	Require(LoadSettings(file) == CompanionSettings{}, "Missing config has defaults");
	CompanionSettings settings;
	settings.scale = 1.375f;
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
	SaveSettings(file, settings);
	Require(LoadSettings(file) == settings, "Every profile/layout field round trips exactly");
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
#endif
} // namespace
int main(int argc, char** argv)
{
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
