#include "Pch.h"
#include "control/legacy/WindowsRuntimeSettings.h"
#include "platform/windows/WindowsRuntimeInput.h"
#include "platform/windows/WindowsRuntimePaths.h"
#include <windows.h>

#include <cstdio>
#include <cstdlib>
#include <filesystem>

namespace fs = std::filesystem;

static int s_failures = 0;

static void Check(bool condition, const char* message)
{
	if (condition)
		return;
	std::fprintf(stderr, "FAIL: %s\n", message);
	++s_failures;
}

static void CheckLaunchResolution()
{
	const auto* width = std::getenv("SIMXR_WIDTH");
	const auto* height = std::getenv("SIMXR_HEIGHT");
	const std::string oldWidth = width ? width : "", oldHeight = height ? height : "";
	Check(_putenv_s("SIMXR_WIDTH", "2048") == 0 && _putenv_s("SIMXR_HEIGHT", "2300") == 0,
		"set per-instance launch resolution");
	simxr::windows::WindowsRuntimeInput launched;
	Check(_putenv_s("SIMXR_WIDTH", "4096") == 0 && _putenv_s("SIMXR_HEIGHT", "4096") == 0,
		"change environment after instance creation");
	const auto snapshot = launched.Snapshot();
	Check(snapshot.views.outerWidth == 2048 && snapshot.views.outerHeight == 2300,
		"Windows launch resolution was ignored or changed after creation");
	Check(_putenv_s("SIMXR_WIDTH", oldWidth.c_str()) == 0 && _putenv_s("SIMXR_HEIGHT", oldHeight.c_str()) == 0,
		"restore launch environment");
}

int main()
{
	const fs::path root = fs::temp_directory_path() / ("simxr-input-test-" + std::to_string(GetCurrentProcessId()));
	const fs::path data = root / "OpenXR-Simulator";
	fs::create_directories(data);
	SetEnvironmentVariableW(L"SIMXR_DATA_DIR", nullptr);
	Check(SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str()) != 0, "isolated data directory was selected");
	Check(fs::path(simxr::windows::RuntimeDataDirectory()) == data, "runtime uses isolated data directory");

	CheckLaunchResolution();
	fs::remove_all(root);
	if (s_failures != 0)
		return 1;
	std::puts("windows_runtime_input: PASS");
	return 0;
}
