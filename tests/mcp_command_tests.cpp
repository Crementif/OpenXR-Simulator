#include "control/legacy/WindowsLegacyChannel.h"
#include "control/legacy/WindowsRuntimeSettings.h"
#include "platform/windows/WindowsRuntimeInput.h"
#include "platform/windows/WindowsRuntimePaths.h"
#include <atomic>
#include <latch>
#include <thread>
#include <windows.h>
#include "TestSupport.h"

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

static void CheckInputOwners(mcp::WindowsLegacyChannel& channel, const fs::path& data)
{
	simxr::legacy::settings::ApplySettingsJson(R"({"input_polling":"preview"})");
	simxr::windows::WindowsRuntimeInput first;
	const auto original = first.Snapshot();
	WriteCommand(data / "head_pose_command.json", R"({"x":3,"y":6,"z":9,"yaw":0.2,"roll":0.4})");
	WriteCommand(data / "fov_command.json",
				 R"({"left":{"aL":-0.3,"aR":0.4,"aU":0.5,"aD":-0.6},"right":{"aL":-0.7,"aR":0.8,"aU":0.9,"aD":-1}})");
	Eventually(
		[&]
		{
			const auto input = first.Poll(std::chrono::milliseconds(10), channel);
			return input.head.x == 3 && input.views.outerFov[0].angleLeft == -0.3f;
		},
		"Instance input did not consume staged head and FOV commands");
	const auto observed = first.Snapshot();
	Check(observed.tracking.head.pose.position.y == 6 && observed.head.roll == 0.4f &&
			  observed.views.outerFov[1].angleRight == 0.8f,
		  "Input snapshot mixed command components");

	simxr::windows::WindowsRuntimeInput recreated;
	const auto fresh = recreated.Snapshot();
	Check(fresh.head.x == original.head.x && fresh.head.roll == original.head.roll &&
			  fresh.views.outerFov[0].angleLeft == original.views.outerFov[0].angleLeft,
		  "Recreated instance inherited another instance's head or FOV override");
	auto pose = first.Head();
	pose.hasRoll = false;
	pose.roll = -1;
	first.SetHead(pose);
	Check(first.Head().roll == 0.4f, "Capture head step without roll discarded the current tilt");

	std::atomic<bool> inconsistent = false;
	std::latch start(2);
	std::jthread reader(
		[&](std::stop_token stop)
		{
			start.arrive_and_wait();
			while (!stop.stop_requested())
			{
				const auto input = first.Snapshot();
				if (input.head.y != input.head.x * 2 || input.head.z != input.head.x * 3 ||
					input.tracking.head.pose.position.x != input.head.x ||
					input.tracking.head.pose.position.y != input.head.y ||
					input.tracking.head.pose.position.z != input.head.z)
					inconsistent = true;
			}
		});
	start.arrive_and_wait();
	for (unsigned index = 0; index < 1000; ++index)
	{
		pose.x = static_cast<float>(index);
		pose.y = pose.x * 2;
		pose.z = pose.x * 3;
		first.SetHead(pose);
	}
	reader.request_stop();
	reader.join();
	Check(!inconsistent && recreated.Head().x == original.head.x && observed.head.x == 3,
		  "Capture edits changed an independent owner or a copied snapshot");
}

int main() {
    const fs::path root = fs::temp_directory_path() / ("simxr-mcp-test-" + std::to_string(GetCurrentProcessId()));
    const fs::path data = root / "OpenXR-Simulator";
    fs::create_directories(data);
    SetEnvironmentVariableW(L"SIMXR_DATA_DIR", nullptr);
    Check(SetEnvironmentVariableW(L"LOCALAPPDATA", root.c_str()) != 0, "isolated command directory was selected");
    Check(fs::path(simxr::windows::RuntimeDataDirectory()) == data, "runtime uses isolated command directory");

    {
        mcp::WindowsLegacyChannel channel(data);
        const auto takeHold = [&]
        {
            mcp::ControllerHoldCommand result;
            Eventually([&] { result = channel.TakeCommands().controllerHold; return result.valid; },
                       "Worker did not stage controller command");
            return result;
        };
        const auto takeWheel = [&]
        {
            mcp::WheelSweepCommand result;
            Eventually([&] { result = channel.TakeCommands().wheel; return result.valid; },
                       "Worker did not stage wheel command");
            return result;
        };
        const fs::path holdPath = data / "controller_hold_command.json";
        WriteCommand(holdPath, R"({"hand":1,"trigger":0.8,"stickX":-1.0})");
        const mcp::ControllerHoldCommand hold = takeHold();
        Check(hold.valid && !hold.clear && hold.hand == 1 && hold.trigger == 0.8f && hold.stickX == -1.0f,
              "held controller values and bipolar stick parse");
        Check(!fs::exists(holdPath), "hold command is consumed once");

        WriteCommand(holdPath, R"({"clear":true})");
        Check(takeHold().clear, "JSON boolean true clears held controls");
        WriteCommand(holdPath, R"({"clear":1})");
        Check(takeHold().clear, "numeric clear remains supported");

        const fs::path wheelPath = data / "wheel_sweep_command.json";
        WriteCommand(wheelPath, R"({"enabled":false})");
        const mcp::WheelSweepCommand disabled = takeWheel();
        Check(disabled.valid && !disabled.enabled, "JSON boolean false stops wheel sweep");
        WriteCommand(wheelPath, R"({"enabled":0})");
        Check(!takeWheel().enabled, "numeric zero still stops wheel sweep");
        WriteCommand(wheelPath, R"({"enabled":true,"radius":0.24,"freqHz":0.3})");
        const mcp::WheelSweepCommand enabled = takeWheel();
        Check(enabled.valid && enabled.enabled && enabled.radius > 0.239f && enabled.freqHz > 0.299f,
              "JSON boolean true and wheel geometry parse");
        CheckInputOwners(channel, data);
    }
    fs::remove_all(root);
    if (failures != 0) return 1;
    std::puts("mcp_command_tests: PASS");
    return 0;
}
