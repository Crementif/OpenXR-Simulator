#include "../TestSupport.h"
#include "control/legacy/LegacyFileChannel.h"

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <limits>
#include <sstream>
#include <unistd.h>

namespace simxr::tests
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

std::string Read(const fs::path& path)
{
	std::ifstream file(path, std::ios::binary);
	return std::string(std::istreambuf_iterator<char>(file), {});
}

void TestInitialSequenceLifetime()
{
	Directory directory;
	const auto sequencePath = directory.Path() / "poses.jsonl";
	{
		std::ofstream sequence(sequencePath);
		sequence << "{\"frame\":1,\"head\":{\"x\":2},\"capture\":true}\n";
	}
	LegacyFileChannel channel(directory.Path(), sequencePath);
	fs::remove(sequencePath);
	auto sequence = channel.TakeSequence();
	Require(sequence && sequence->Size() == 1, "Initial sequence was not loaded");
	LegacyInput input;
	input.StartSequence(std::move(*sequence), {40});
	Require(!input.AdvanceSequence({40}) && input.AdvanceSequence({41}) && input.HeadPose().position.x == 2,
			"Prepared sequence retained a dependency on its deleted source file");
	Require(!channel.TakeSequence(), "Initial sequence was delivered more than once");
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
	const auto previousErrors = channel.Status().errors;
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
} // namespace simxr::tests

int main()
{
	using namespace simxr::tests;
	try
	{
		TestInitialSequenceLifetime();
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
