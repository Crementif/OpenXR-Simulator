#include "../TestSupport.h"
#include "control/legacy/LegacyFileChannel.h"
#include "json.h"
#include "platform/linux/LinuxRuntimeStatus.h"
#include "runtime/Session.h"

#include <filesystem>
#include <fstream>
#include <iostream>
#include <locale>
#include <string>
#include <unistd.h>

namespace
{
using namespace simxr;
using namespace std::chrono_literals;
namespace fs = std::filesystem;

class StatusFiles final
{
  public:
	StatusFiles()
		: m_directory(fs::temp_directory_path() /
					  ("simxr-status-" + std::to_string(getpid()) + "-" +
					   std::to_string(LinuxRuntimeStatus::Clock::now().time_since_epoch().count()))),
		  m_channel(std::make_shared<LegacyFileChannel>(m_directory))
	{
	}
	~StatusFiles()
	{
		m_channel->Close();
		std::error_code ignored;
		fs::remove_all(m_directory, ignored);
	}
	std::shared_ptr<LegacyFileChannel> Channel() const
	{
		return m_channel;
	}
	std::string Read(const char* name) const
	{
		std::ifstream file(m_directory / name, std::ios::binary);
		return std::string(std::istreambuf_iterator<char>(file), {});
	}
	void Flush()
	{
		const auto marker = std::to_string(++m_flush);
		Require(m_channel->Publish(LegacyOutputFile::ProjectionLog, marker), "Queue status writer marker");
		Eventually(
			[&]
			{
				return Read("projection_log.json") == marker;
			},
			"Status writer did not reach marker");
	}

  private:
	const fs::path m_directory;
	const std::shared_ptr<LegacyFileChannel> m_channel;
	std::uint32_t m_flush = 0;
};

SessionSnapshot RunningSession(std::uint64_t generation, std::uint64_t frame)
{
	SessionSnapshot session;
	session.running = true;
	session.generation = {generation};
	session.lastSubmitted = {frame};
	session.lastIssued.displayTime = {Duration{123456789}};
	return session;
}

void CheckCadenceAndGeneration()
{
	StatusFiles files;
	LinuxRuntimeStatus status(files.Channel(), 42, ExecutionMode::RealTime);
	LegacyInput input;
	const auto start = LinuxRuntimeStatus::Clock::time_point{1000ms};
	auto session = RunningSession(1, 10);
	status.Update(session, input, start, LinuxRuntimeStatus::FrameCounts{3, 2});
	files.Flush();
	const auto first = files.Read("frame_info.json");
	Require(json::Object(first.c_str()).number("frame_count", -1) == 10,
			"First generation observation is published immediately");
	session.lastSubmitted = {11};
	status.Update(session, input, start + 10ms, LinuxRuntimeStatus::FrameCounts{2, 1});
	session.lastSubmitted = {12};
	status.Update(session, input, start + 20ms, LinuxRuntimeStatus::FrameCounts{2, 1});
	status.Update(session, input, start + 499ms);
	files.Flush();
	Require(files.Read("frame_info.json") == first, "Real-time status remains throttled within 500 ms");
	status.Update(session, input, start + 510ms);
	files.Flush();
	const auto measured = files.Read("frame_info.json");
	const json::Object performance(measured.c_str());
	Require(performance.number("frame_count", -1) == 12 && performance.number("fps", -1.0) == 100.0 &&
				performance.number("predicted_display_time", -1) == 123456789,
			"Every accepted projection frame contributes to cadence even while status is throttled");
	session = RunningSession(2, 1);
	status.Update(session, input, start + 511ms, LinuxRuntimeStatus::FrameCounts{1, 1});
	files.Flush();
	const auto restarted = files.Read("frame_info.json");
	const json::Object restartedFrame(restarted.c_str());
	Require(restartedFrame.number("frame_count", -1) == 1 && restartedFrame.number("fps", -1.0) == 0,
			"Replacement session resets publication admission and previous frame samples");
	session.running = false;
	session.lastSubmitted = {2};
	status.Update(session, input, start + 512ms);
	files.Flush();
	const auto idle = files.Read("runtime_status.json");
	Require(json::Object(idle.c_str()).string("session_state") == "IDLE",
			"Lifecycle status bypasses the running-session throttle");
}

class CommaPunctuation final : public std::numpunct<char>
{
  private:
	char do_decimal_point() const override
	{
		return ',';
	}
};

class HostLocale final
{
  public:
	HostLocale() : m_previous(std::locale::global(std::locale(std::locale::classic(), new CommaPunctuation))) {}
	~HostLocale()
	{
		std::locale::global(m_previous);
	}

  private:
	const std::locale m_previous;
};

void CheckDeterministicAndInstanceLifetime()
{
	StatusFiles files;
	LegacyInput input;
	input.Apply(LegacyInputCommand::HeadPose, R"({"x":0.25,"yaw":0.5})");
	const auto now = LinuxRuntimeStatus::Clock::time_point{1000ms};
	auto session = RunningSession(7, 1);
	{
		LinuxRuntimeStatus status(files.Channel(), 64, ExecutionMode::Deterministic);
		status.Update(session, input, now, LinuxRuntimeStatus::FrameCounts{1, 1});
		HostLocale locale;
		session.lastSubmitted = {2};
		status.Update(session, input, now + 10ms, LinuxRuntimeStatus::FrameCounts{4, 2});
		files.Flush();
		const auto document = files.Read("frame_info.json");
		const json::Object frame(document.c_str());
		Require(frame.number("frame_count", -1) == 2 && frame.number("layer_count", -1) == 4 &&
					frame.number("projection_layers", -1) == 2 && frame.number("fps", -1.0) == 100.0,
				"Deterministic status publishes every accepted frame with locale-independent numbers");
		const auto runtime = files.Read("runtime_status.json");
		const json::Object state(runtime.c_str());
		Require(state.number("pid", -1) == 64 &&
					state.object("head_tracking").object("position").number("x", -1.0) == 0.25,
				"Runtime status preserves process and input values under a host locale");
	}
	LinuxRuntimeStatus replacement(files.Channel(), 65, ExecutionMode::RealTime);
	replacement.Update(session, input, now + 11ms, LinuxRuntimeStatus::FrameCounts{1, 1});
	files.Flush();
	const auto frame = files.Read("frame_info.json");
	const auto runtime = files.Read("runtime_status.json");
	Require(json::Object(frame.c_str()).number("fps", -1.0) == 0 &&
				json::Object(runtime.c_str()).number("pid", -1) == 65,
			"Replacement instance owns independent cadence and publication state");
	files.Channel()->Close();
	replacement.Update(session, input, now + 1s, LinuxRuntimeStatus::FrameCounts{1, 1});
	Require(files.Channel()->Status().closed, "Optional publication tolerates a retired file worker");
}
} // namespace

int main()
{
	try
	{
		CheckCadenceAndGeneration();
		CheckDeterministicAndInstanceLifetime();
		std::cout << "Linux runtime status checks passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
