#include "mcp_integration.h"
#include "control/legacy/WindowsRuntimeSettings.h"
namespace settings = simxr::legacy::settings;

#include <cstdio>
#include <stdexcept>
#include <string_view>

namespace
{
void Require(bool condition, const char* message)
{
	if (!condition)
		throw std::runtime_error(message);
}
} // namespace

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
		const auto& directory = mcp::GetSimulatorDataDirectory();
		Require(directory == expected, "Runtime data directory ignored environment precedence");
        std::filesystem::create_directories(directory);
        FILE* file = nullptr;
        Require(_wfopen_s(&file, (directory / L"settings.json").c_str(), L"wb") == 0 && file,
                "Open Unicode settings path");
        const char json[] = "{\"render_width\": 1500, \"render_height\": 1600}";
        const bool written = fwrite(json, 1, sizeof(json) - 1, file) == sizeof(json) - 1;
        const bool closed = fclose(file) == 0;
        Require(written && closed, "Write compatibility settings fixture");
        settings::LoadSettings(directory);
        Require(settings::ReadState().renderWidth == 1500, "Unicode settings did not load");
		std::filesystem::current_path(previous);
		std::filesystem::remove_all(temporary);
		std::puts("PASS Windows data directory precedence and Unicode settings load");
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
