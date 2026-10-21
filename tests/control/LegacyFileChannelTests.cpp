#include "../TestSupport.h"
#include "control/legacy/LegacyFileChannel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <sys/stat.h>
#include <unistd.h>

namespace
{
using namespace simxr;
namespace fs = std::filesystem;

class Directory
{
  public:
	Directory()
		: m_path(fs::temp_directory_path() /
				 ("simxr-legacy-" + std::to_string(getpid()) + "-" +
				  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())))
	{
		fs::create_directories(m_path);
	}
	~Directory()
	{
		std::error_code ignored;
		fs::remove_all(m_path, ignored);
	}
	const fs::path& Path() const
	{
		return m_path;
	}

  private:
	fs::path m_path;
};

std::string Receipt(const fs::path& path)
{
	struct stat information{};
	Require(stat(path.c_str(), &information) == 0, "Inspect command receipt identity");
	return std::to_string(information.st_dev) + ":" + std::to_string(information.st_ino) + ":" +
		   std::to_string(information.st_mtim.tv_sec) + ":" + std::to_string(information.st_mtim.tv_nsec);
}

std::string Publish(const fs::path& directory, const char* name, const std::string& text)
{
	const auto temporary = directory / "producer.tmp";
	{
		std::ofstream file(temporary, std::ios::binary);
		file << text;
		Require(file.good(), "Prepare legacy command");
	}
	const auto receipt = Receipt(temporary);
	fs::rename(temporary, directory / name);
	return receipt;
}

void PublishCommand(const fs::path& directory, const char* name, const std::string& text)
{
	const auto receipt = Publish(directory, name, text);
	Eventually(
		[&]
		{
			std::ifstream file(directory / (std::string(name) + ".received"));
			const std::string observed(std::istreambuf_iterator<char>(file), {});
			return observed == receipt;
		},
		"Worker did not stage the exact command independently of frames");
}

std::string Read(const fs::path& path)
{
	std::ifstream file(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file), {});
}

void TestStagingAndSequenceLifetime()
{
	Directory directory;
	LegacyFileChannel channel(directory.Path());
	PublishCommand(directory.Path(), "head_pose_command.json", R"({"x":0.2})");
	PublishCommand(directory.Path(), "head_pose_command.json", R"({"x":0.8})");
	PublishCommand(directory.Path(), "controller_hold_command.json", R"({"hand":1,"trigger":0.7})");
	const auto sequencePath = directory.Path() / "poses.jsonl";
	{
		std::ofstream sequence(sequencePath);
		sequence << "{\"frame\":1,\"head\":{\"x\":2},\"capture\":true}\n";
	}
	PublishCommand(directory.Path(), "sequence_command.json", "{\"path\":\"" + sequencePath.string() + "\"}");
	fs::remove(sequencePath);
	PublishCommand(directory.Path(), "screenshot_request.json", R"({"eye":"right","layer":"quad"})");
	PublishCommand(directory.Path(), "projection_log_dump_request", "");
	const auto requests = channel.TakeRequests();
	Require(requests.capture && requests.capture->eye == "right" && requests.capture->layer == "quad" &&
				requests.projectionLog,
			"Explicit output requests were not staged separately");
	auto commands = channel.TakeCommands();
	Require(commands.input[0] && commands.input[4] && commands.sequence && commands.sequence->Size() == 1,
			"Taking output requests consumed input commands");
	LegacyInput input;
	input.Apply(LegacyInputCommand::HeadPose, *commands.input[0]);
	input.Apply(LegacyInputCommand::ControllerHold, *commands.input[4]);
	Require(input.HeadPose().position.x == 0.8f && input.Controls()[1].trigger == 0.7f,
			"Latest one-shot input values were not preserved");
	input.StartSequence(std::move(*commands.sequence), {40});
	Require(!input.AdvanceSequence({40}) && input.AdvanceSequence({41}) && input.HeadPose().position.x == 2,
			"Prepared sequence retained a dependency on its deleted source file");
	Require(!channel.TakeCommands().input[0] && !channel.TakeRequests().capture,
			"A consumed request was delivered more than once");
}

void TestConcurrentReplacement()
{
	Directory directory;
	LegacyFileChannel channel(directory.Path());
	std::string expected;
	for (unsigned index = 0; index < 100; ++index)
		expected = Publish(directory.Path(), "head_pose_command.json",
						   "{\"x\":" + std::to_string(index) + ",\"padding\":\"" + std::string(32000, 'x') + "\"}");
	Eventually(
		[&]
		{
			return Read(directory.Path() / "head_pose_command.json.received") == expected;
		},
		"A claimed older command deleted a newer atomic replacement");
	auto commands = channel.TakeCommands();
	Require(commands.input[0].has_value(), "Latest replacement did not reach the input batch");
	LegacyInput input;
	input.Apply(LegacyInputCommand::HeadPose, *commands.input[0]);
	Require(input.HeadPose().position.x == 99, "Older command publication replaced the final update");
}

void TestOutputsAndShutdown()
{
	Directory directory;
	const auto output = directory.Path() / "données-画面";
	{
		LegacyFileChannel channel(output);
		Require(channel.Publish(LegacyOutputFile::RuntimeStatus, "old") &&
					channel.Publish(LegacyOutputFile::RuntimeStatus, "latest") &&
					channel.Publish(LegacyOutputFile::FrameInfo, "frames") && channel.Log("joined writer"),
				"Accept bounded legacy output values");
		LegacyScreenshot screenshot{2, 1, {1, 2, 3, 255, 4, 5, 6, 255}, "{\"capturedFrame\":7}"};
		Require(channel.SaveScreenshot(screenshot) && screenshot.bgra.empty(),
				"Accepted screenshot did not transfer CPU bytes");
		channel.Close();
		Require(channel.Status().closed && channel.Status().errors == 0 &&
					!channel.Publish(LegacyOutputFile::FrameInfo, "late"),
				"Closed worker accepted another file job");
		LegacyScreenshot rejected{1, 1, {7, 8, 9, 255}, "late"};
		Require(!channel.SaveScreenshot(rejected) && rejected.bgra.size() == 4,
				"Rejected screenshot consumed its caller's bytes");
	}
	Require(Read(output / "runtime_status.json") == "latest" && Read(output / "frame_info.json") == "frames",
			"Shutdown did not flush the latest coalesced status values");
	const auto bitmap = Read(output / "screenshot.bmp");
	Require(bitmap.size() == 62 && bitmap[0] == 'B' && bitmap[1] == 'M' && bitmap[54] == 1 && bitmap[58] == 4 &&
				Read(output / "screenshot_status.json") == "{\"capturedFrame\":7}",
			"Screenshot pixels or completion metadata changed");
	Require(Read(output / ("openxr_simulator." + std::to_string(getpid()) + ".log")).find("joined writer") !=
				std::string::npos,
			"Accepted log did not finish before shutdown");
}

void TestFailureRecoveryAndBounds()
{
	Directory directory;
	LegacyFileChannel channel(directory.Path());
	Require(mkfifo((directory.Path() / "pipe.tmp").c_str(), 0600) == 0, "Prepare nonregular command");
	fs::rename(directory.Path() / "pipe.tmp", directory.Path() / "head_pose_command.json");
	Eventually(
		[&]
		{
			return channel.Status().errors != 0;
		},
		"Nonregular command blocked the worker");
	Require(!fs::exists(directory.Path() / "head_pose_command.json.received"),
			"Rejected file claimed a staged receipt");
	PublishCommand(directory.Path(), "head_pose_command.json", R"({"x":3})");
	Require(channel.TakeCommands().input[0].has_value(), "Rejected command prevented later valid staging");
	const auto retained = directory.Path() / ("head_pose_command.json.pending." + std::to_string(getpid()));
	{
		std::ofstream preserved(retained);
		preserved << "recoverable";
	}
	const auto beforeClaimFailure = channel.Status().errors;
	Publish(directory.Path(), "head_pose_command.json", R"({"x":4})");
	Eventually(
		[&]
		{
			return channel.Status().errors > beforeClaimFailure;
		},
		"A retained failed claim was overwritten");
	Require(Read(retained) == "recoverable" && fs::exists(directory.Path() / "head_pose_command.json"),
			"Failed claim recovery destroyed a retained or newer command file");
	fs::remove(retained);
	PublishCommand(directory.Path(), "head_pose_command.json", R"({"x":5})");
	Require(channel.TakeCommands().input[0].has_value(), "Command staging did not recover after retained-file repair");
	const auto previousErrors = channel.Status().errors;
	Publish(directory.Path(), "screenshot_request.json", R"({"eye":"invalid","layer":"all"})");
	Eventually(
		[&]
		{
			return channel.Status().errors > previousErrors;
		},
		"Invalid capture selection was not rejected");
	Require(!channel.TakeRequests().capture && !fs::exists(directory.Path() / "screenshot_request.json.received"),
			"Invalid capture selection reached a frame or received a staging acknowledgement");
	fs::create_directory(directory.Path() / "screenshot.bmp");
	LegacyScreenshot failed{1, 1, {1, 2, 3, 255}, "failed"};
	Require(channel.SaveScreenshot(failed) && channel.DrainScreenshots(std::chrono::seconds(3)),
			"Failed file job did not settle");
	Require(channel.Status().errors > previousErrors && !fs::exists(directory.Path() / "screenshot_status.json"),
			"Failed image publication claimed a successful screenshot status");
	Require(!fs::exists(directory.Path() / ("screenshot.bmp.tmp." + std::to_string(getpid()))),
			"Failed atomic file publication abandoned its temporary file");
	fs::remove(directory.Path() / "screenshot.bmp");
	LegacyScreenshot recovered{1, 1, {4, 5, 6, 255}, "recovered"};
	Require(channel.SaveScreenshot(recovered) && channel.DrainScreenshots(std::chrono::seconds(3)) &&
				Read(directory.Path() / "screenshot_status.json") == "recovered",
			"Failed screenshot permanently consumed capacity");
	LegacyScreenshot invalid{
		std::numeric_limits<std::uint32_t>::max(), std::numeric_limits<std::uint32_t>::max(), {}, ""};
	bool rejected = false;
	try
	{
		channel.SaveScreenshot(invalid);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && !channel.Publish(LegacyOutputFile::FrameInfo, std::string(1024 * 1024 + 1, 'x')),
			"Oversized output or overflowing screenshot dimensions were admitted");
}
} // namespace

int main()
{
	try
	{
		TestStagingAndSequenceLifetime();
		TestConcurrentReplacement();
		TestOutputsAndShutdown();
		TestFailureRecoveryAndBounds();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
