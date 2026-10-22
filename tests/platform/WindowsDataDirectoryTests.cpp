#include "mcp_integration.h"
#include "ui_enhancements.h"

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
		ui::LoadSettings(directory);
		ui::g_uiState.previewFps = 30;
		ui::SaveSettings();
		Require(std::filesystem::exists(directory / L"settings.json"), "Settings did not use the native data path");
		ui::g_uiState.previewFps = 90;
		ui::LoadSettings(directory);
		Require(ui::g_uiState.previewFps == 30, "Unicode settings did not reload");
		std::filesystem::current_path(previous);
		std::filesystem::remove_all(temporary);
		std::puts("PASS Windows data directory precedence and Unicode settings round trip");
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
