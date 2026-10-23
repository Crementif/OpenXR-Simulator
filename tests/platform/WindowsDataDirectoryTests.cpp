#include "Pch.h"
#include "control/legacy/WindowsRuntimeSettings.h"
#include "platform/windows/WindowsRuntimePaths.h"
#include <windows.h>
namespace settings = simxr::legacy::settings;

#include <cstdio>
#include <fstream>
#include <stdexcept>
#include <string_view>

static void Require(bool condition, const char* message)
{
	if (!condition)
		throw std::runtime_error(message);
}

int main(int argc, char** argv)
{
	std::filesystem::path temporary;
	const auto previous = std::filesystem::current_path();
	try
	{
		Require(argc == 2, "Expected override, default or fallback mode");
		wchar_t base[MAX_PATH + 1]{};
		Require(GetTempPathW(MAX_PATH + 1, base) != 0, "Read temporary directory");
		temporary = std::filesystem::path(base) / (L"simxr-data-" + std::to_wstring(GetCurrentProcessId()));
		std::filesystem::create_directories(temporary);
		std::filesystem::current_path(temporary);
		const auto local = temporary / L"local-\u4e2d\u6587";
		const auto overridePath = temporary / L"override-\u03bb" / L"nested-\u65e5\u672c";
		const std::string_view mode = argv[1];
		Require(mode == "override" || mode == "default" || mode == "fallback", "Unknown mode");
		Require(SetEnvironmentVariableW(L"LOCALAPPDATA", mode == "fallback" ? nullptr : local.c_str()),
				"Set local data root");
		Require(SetEnvironmentVariableW(L"SIMXR_DATA_DIR", mode == "override" ? overridePath.c_str() : nullptr),
				"Set explicit data directory");
		const auto expected = mode == "override"  ? overridePath
							  : mode == "default" ? local / L"OpenXR-Simulator"
												  : std::filesystem::path(L".") / L"OpenXR-Simulator";
		const auto& directory = simxr::windows::RuntimeDataDirectory();
		Require(directory == expected, "Runtime data directory ignored environment precedence");
		settings::LoadSettings(directory);
		Require(settings::ReadState().renderWidth == 1280, "Missing settings changed startup defaults");
		const auto path = directory / "settings.json";
		const auto writeSettings = [&](std::string_view text)
		{
			std::ofstream file(path, std::ios::binary | std::ios::trunc);
			Require(static_cast<bool>(file), "Open Unicode settings path");
			file.write(text.data(), static_cast<std::streamsize>(text.size()));
			Require(static_cast<bool>(file), "Write compatibility settings fixture");
			file.close();
			Require(static_cast<bool>(file), "Close compatibility settings fixture");
		};
		const auto requireRejectedLoad = [&]
		{
			bool rejected = false;
			try
			{
				settings::LoadSettings(directory);
			}
			catch (const std::exception&)
			{
				rejected = true;
			}
			Require(rejected, "Invalid settings input was silently accepted");
			const auto state = settings::ReadState();
			Require(state.renderWidth == 1500 && state.renderHeight == 1600,
					"Failed settings load partially changed the active resolution");
		};

		writeSettings("{\"padding\":\"" + std::string(4096, ' ') + "\",\"render_width\":1500,\"render_height\":1600}");
		settings::LoadSettings(directory);
		Require(settings::ReadState().renderWidth == 1500, "Unicode settings were truncated at 2 KiB");

		writeSettings("{\"render_width\":2200,\"render_height\":2300,\"padding\":\"" + std::string(1024 * 1024, ' ') +
					  "\"}");
		requireRejectedLoad();
		std::filesystem::remove(path);
		std::filesystem::create_directory(path);
		requireRejectedLoad();
		std::filesystem::remove(path);
		settings::LoadSettings(directory);
		Require(settings::ReadState().renderWidth == 1500, "Missing settings reset existing values");
		std::filesystem::current_path(previous);
		std::filesystem::remove_all(temporary);
		std::puts("PASS Windows data directory precedence and complete Unicode settings load");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::filesystem::current_path(previous);
		if (!temporary.empty())
			std::filesystem::remove_all(temporary);
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
