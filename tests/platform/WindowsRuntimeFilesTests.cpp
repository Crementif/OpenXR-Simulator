#include "../TestSupport.h"
#include "Pch.h"
#include "platform/windows/WindowsRuntimeStatus.h"
#include "control/legacy/WindowsLegacyFiles.h"
#include "json.h"
#include <iostream>

namespace simxr::windows::tests
{
namespace fs = std::filesystem;
class Directory
{
  public:
	Directory()
		: m_path(fs::temp_directory_path() /
				 ("simxr-windows-compatibility-" +
				  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count())) /
				 fs::path(u8"données-画面"))
	{
		fs::create_directories(m_path);
	}
	~Directory()
	{
		std::error_code ignored;
		fs::remove_all(m_path.parent_path(), ignored);
	}
	const fs::path& Path() const
	{
		return m_path;
	}

  private:
	fs::path m_path;
};
static std::string Read(const fs::path& path)
{
	std::ifstream file(path, std::ios::binary);
	return {std::istreambuf_iterator<char>(file), {}};
}
static void TestClassicJson()
{
	Directory directory;
	auto files = std::make_shared<WindowsRuntimeFiles>(directory.Path());
	struct CommaDecimal : std::numpunct<char>
	{
		char do_decimal_point() const override
		{
			return ',';
		}
	};
	class LocaleScope
	{
	  public:
		LocaleScope() : m_previous(std::locale::global(std::locale(std::locale::classic(), new CommaDecimal))) {}
		~LocaleScope()
		{
			std::locale::global(m_previous);
		}

	  private:
		std::locale m_previous;
	} locale;
	WindowsRuntimeStatus publisher(files);
	FrameStatus status;
	status.frame = (std::uint64_t{1} << 40) + 19;
	status.yaw = .25f;
	status.format = "format\"\\";
	status.state = "state\n";
	publisher.PublishStatus(status);
	files->Close();
	const auto text = Read(directory.Path() / "runtime_status.json");
	Require(json::Object(text.c_str()).String("format") == status.format &&
				json::Object(text.c_str()).String("session_state") == status.state,
			"Runtime status did not escape text");
	Require(text.find("\"yaw\":0.25") != std::string::npos && text.find("\"timestamp\":") != std::string::npos,
			"Ambient locale or status preparation changed the legacy JSON contract");
	Require(text.find("\"frame_count\":" + std::to_string(status.frame)) != std::string::npos,
			"Runtime status narrowed its frame identity");
}

static void TestStatusFailureRecoveryAndShutdown()
{
	Directory directory;
	WindowsRuntimeFiles files(directory.Path());
	const auto destination = directory.Path() / "runtime_status.json";
	fs::create_directory(destination);
	Require(files.PublishStatus("failed"), "Accept failing status");
	Eventually([&] { return files.Status().errors != 0; }, "Status failure was not reported");
	Require(!files.Status().detail.empty(), "Status failure omitted its detail");
	fs::remove(destination);
	Require(files.PublishStatus("recovered") && files.Log("joined output"), "Accept recovery output");
	files.Close();
	Require(Read(destination) == "recovered" && files.Status().closed &&
			!files.PublishStatus("late") && !files.Log("late"),
			"Shutdown did not drain output or reject later work");
	Require(Read(directory.Path() / ("openxr_simulator." + std::to_string(mcp::WindowsLegacyProcessId()) + ".log"))
			.find("joined output") != std::string::npos, "Shutdown lost accepted log");
	for (const auto& entry : fs::directory_iterator(directory.Path()))
		Require(entry.path().filename().string().find(".tmp.") == std::string::npos,
				"Failed status publication left a temporary file");
}

static void TestIndependentAtomicOutputs()
{
	Directory directory;
	const auto destination = directory.Path() / "runtime_status.json";
	auto stale = destination;
	stale += ".tmp." + std::to_string(mcp::WindowsLegacyProcessId());
	{
		std::ofstream previous(stale);
		previous << "earlier output";
	}
	const std::string_view firstText = "first complete document";
	const std::string_view secondText = "second complete document with different length";
	{
		mcp::WindowsLegacyOutput first(destination);
		mcp::WindowsLegacyOutput second(destination);
		first.Write(std::as_bytes(std::span(firstText)));
		second.Write(std::as_bytes(std::span(secondText)));
		first.Commit();
		Require(Read(destination) == firstText, "Second output modified another owner's pending bytes");
		second.Commit();
		Require(Read(destination) == secondText, "Independent pending output could not replace its destination");
		mcp::WindowsLegacyOutput abandoned(destination);
		abandoned.Write(std::as_bytes(std::span(firstText)));
	}
	Require(Read(destination) == secondText && Read(stale) == "earlier output",
			"Abandoned publication changed completed or previously existing output");
	fs::remove(stale);
	for (const auto& entry : fs::directory_iterator(directory.Path()))
		Require(entry.path() == destination, "Independent atomic publication left temporary files behind");
}

#ifndef _WIN32
static void TestLogCloseFailure()
{
	Directory directory;
	const auto path = directory.Path() / ("openxr_simulator." + std::to_string(mcp::WindowsLegacyProcessId()) + ".log");
	fs::create_symlink("/dev/full", path);
	WindowsRuntimeFiles files(directory.Path());
	Require(files.Log("buffered output"), "Accept log for close failure fixture");
	files.Close();
	Require(files.Status().errors != 0 && !files.Status().detail.empty(),
			"Buffered log close failure was silently discarded");
}
#endif
} // namespace simxr::windows::tests

int main()
{
	using namespace simxr::windows::tests;
	try
	{
		TestClassicJson();
		TestIndependentAtomicOutputs();
		TestStatusFailureRecoveryAndShutdown();
#ifndef _WIN32
		TestLogCloseFailure();
#endif
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
