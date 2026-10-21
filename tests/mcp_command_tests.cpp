#include "mcp_integration.h"

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace fs = std::filesystem;

static int failures = 0;

static void Check(bool condition, const char* message) {
    if (condition) return;
    std::fprintf(stderr, "FAIL: %s\n", message);
    ++failures;
}

static void WriteCommand(const fs::path& path, const char* json) {
    std::ofstream file(path, std::ios::binary);
    file << json;
    file.close();
    Check(file.good(), "command file was written");
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("simxr-mcp-test-" + std::to_string(GetCurrentProcessId()));
    const fs::path data = root / "OpenXR-Simulator";
    fs::create_directories(data);
    Check(SetEnvironmentVariableA("LOCALAPPDATA", root.string().c_str()) != 0, "isolated command directory was selected");
    Check(fs::path(mcp::GetSimulatorDataPath()) == data, "runtime uses isolated command directory");

    const fs::path holdPath = data / "controller_hold_command.json";
    WriteCommand(holdPath, R"({"hand":1,"trigger":0.8,"stickX":-1.0})");
    const mcp::ControllerHoldCommand hold = mcp::CheckControllerHoldCommand();
    Check(hold.valid && !hold.clear && hold.hand == 1 && hold.trigger == 0.8f && hold.stickX == -1.0f,
          "held controller values and bipolar stick parse");
    Check(!fs::exists(holdPath), "hold command is consumed once");

    WriteCommand(holdPath, R"({"clear":true})");
    Check(mcp::CheckControllerHoldCommand().clear, "JSON boolean true clears held controls");
    WriteCommand(holdPath, R"({"clear":1})");
    Check(mcp::CheckControllerHoldCommand().clear, "numeric clear remains supported");

    const fs::path wheelPath = data / "wheel_sweep_command.json";
    WriteCommand(wheelPath, R"({"enabled":false})");
    const mcp::WheelSweepCommand disabled = mcp::CheckWheelSweepCommand();
    Check(disabled.valid && !disabled.enabled, "JSON boolean false stops wheel sweep");
    WriteCommand(wheelPath, R"({"enabled":0})");
    Check(!mcp::CheckWheelSweepCommand().enabled, "numeric zero still stops wheel sweep");
    WriteCommand(wheelPath, R"({"enabled":true,"radius":0.24,"freqHz":0.3})");
    const mcp::WheelSweepCommand enabled = mcp::CheckWheelSweepCommand();
    Check(enabled.valid && enabled.enabled && enabled.radius > 0.239f && enabled.freqHz > 0.299f,
          "JSON boolean true and wheel geometry parse");

    fs::remove_all(root);
    if (failures != 0) return 1;
    std::puts("mcp_command_tests: PASS");
    return 0;
}
