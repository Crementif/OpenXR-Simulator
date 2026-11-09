#include "Pch.h"
#include "app/platform/windows/LaunchSupport.h"
#include "app/services/Utf8Path.h"
#include "../TestSupport.h"

#include <nlohmann/json.hpp>

using namespace std::chrono_literals;

static void CheckWindows(BOOL result, const char* operation)
{
	if (!result)
		throw simxr::app::windows::Error(operation);
}

static DWORD RecordProcess(const std::filesystem::path& directory)
{
	std::error_code error;
	for (std::filesystem::directory_iterator entry(directory, error), end; !error && entry != end;
		 entry.increment(error))
	{
		if (entry->path().extension() != L".json")
			continue;
		std::ifstream stream(entry->path(), std::ios::binary);
		const auto value = nlohmann::json::parse(stream, nullptr, false);
		if (value.is_object() && value.contains("process_id") && value["process_id"].is_number_unsigned())
			return value["process_id"].get<DWORD>();
	}
	return 0;
}

static std::string ReadLog(const std::filesystem::path& path)
{
	std::ifstream log(path, std::ios::binary);
	return {(std::istreambuf_iterator<char>(log)), std::istreambuf_iterator<char>()};
}

// Independent: the first tool call starts the app outside the connector's job. Contained: the job forbids
// breakaway, so the app starts inside it and ends with it. Attached: a hand-started app is already
// registered, and the connector uses it without starting another.
enum class Expect
{
	Independent,
	Contained,
	Attached
};

static simxr::app::windows::Handle StartByHand(const std::filesystem::path& app, const std::filesystem::path& directory)
{
	auto command = simxr::app::windows::Quote(app.native()) + L" --mcp";
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	PROCESS_INFORMATION process{};
	CheckWindows(CreateProcessW(app.c_str(), command.data(), nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr,
								nullptr, &startup, &process),
				 "Start the app by hand");
	simxr::app::windows::Handle child(process.hProcess), thread(process.hThread);
	const auto deadline = std::chrono::steady_clock::now() + 15s;
	while (RecordProcess(directory) != process.dwProcessId)
	{
		if (std::chrono::steady_clock::now() >= deadline || WaitForSingleObject(child.Get(), 0) == WAIT_OBJECT_0)
		{
			TerminateProcess(child.Get(), 1);
			throw std::runtime_error("An app hand-started with --mcp did not register for MCP");
		}
		std::this_thread::sleep_for(25ms);
	}
	return child;
}

static void RunCase(const std::filesystem::path& connector, const std::filesystem::path& app,
					const std::filesystem::path& directory, DWORD limits, Expect expect)
{
	using simxr::app::windows::Handle;
	CheckWindows(SetEnvironmentVariableW(L"SIMXR_MCP_DISCOVERY_DIR", directory.c_str()), "Set discovery directory");
	Handle handStarted;
	DWORD handPid = 0;
	if (expect == Expect::Attached)
	{
		handStarted = StartByHand(app, directory);
		handPid = GetProcessId(handStarted.Get());
	}
	Handle job(CreateJobObjectW(nullptr, nullptr));
	if (!job.Get())
		throw simxr::app::windows::Error("Create test job");
	JOBOBJECT_EXTENDED_LIMIT_INFORMATION information{};
	information.BasicLimitInformation.LimitFlags = JOB_OBJECT_LIMIT_KILL_ON_JOB_CLOSE | limits;
	CheckWindows(
		SetInformationJobObject(job.Get(), JobObjectExtendedLimitInformation, &information, sizeof(information)),
		"Configure test job");
	SECURITY_ATTRIBUTES inherited{sizeof(SECURITY_ATTRIBUTES), nullptr, TRUE};
	HANDLE readPipe = nullptr, writePipe = nullptr;
	CheckWindows(CreatePipe(&readPipe, &writePipe, &inherited, 0), "Create connector stdin");
	Handle input(readPipe), writer(writePipe);
	CheckWindows(SetHandleInformation(writer.Get(), HANDLE_FLAG_INHERIT, 0), "Protect connector pipe writer");
	Handle nullFile(CreateFileW(L"NUL", GENERIC_READ | GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE, &inherited,
								OPEN_EXISTING, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (nullFile.Get() == INVALID_HANDLE_VALUE)
		throw simxr::app::windows::Error("Open test output");
	auto logPath = directory;
	logPath += L".connector.log";
	Handle logFile(CreateFileW(logPath.c_str(), GENERIC_WRITE, FILE_SHARE_READ | FILE_SHARE_WRITE | FILE_SHARE_DELETE,
							   &inherited, CREATE_NEW, FILE_ATTRIBUTE_NORMAL, nullptr));
	if (logFile.Get() == INVALID_HANDLE_VALUE)
		throw simxr::app::windows::Error("Open connector test log");
	STARTUPINFOW startup{};
	startup.cb = sizeof(startup);
	startup.dwFlags = STARTF_USESTDHANDLES;
	startup.hStdInput = input.Get();
	startup.hStdOutput = logFile.Get();
	startup.hStdError = logFile.Get();
	auto command =
		simxr::app::windows::Quote(connector.native()) + L" --app " + simxr::app::windows::Quote(app.native());
	PROCESS_INFORMATION process{};
	CheckWindows(CreateProcessW(connector.c_str(), command.data(), nullptr, nullptr, TRUE,
								CREATE_SUSPENDED | CREATE_NEW_PROCESS_GROUP, nullptr, nullptr, &startup, &process),
				 "Create test connector");
	Handle child(process.hProcess), thread(process.hThread), appProcess;
	try
	{
		CheckWindows(AssignProcessToJobObject(job.Get(), child.Get()), "Assign test job");
		if (ResumeThread(thread.Get()) == static_cast<DWORD>(-1))
			throw simxr::app::windows::Error("Resume test connector");
		const std::string requests =
			R"({"jsonrpc":"2.0","id":1,"method":"initialize","params":{"protocolVersion":"2025-11-25","capabilities":{},"clientInfo":{"name":"job-test","version":"1"}}})"
			"\n"
			R"({"jsonrpc":"2.0","method":"notifications/initialized"})"
			"\n"
			R"({"jsonrpc":"2.0","id":2,"method":"tools/call","params":{"name":"get_headset_profiles","arguments":{}}})"
			"\n";
		DWORD written = 0;
		const auto sent =
			WriteFile(writer.Get(), requests.data(), static_cast<DWORD>(requests.size()), &written, nullptr);
		if (!sent)
			throw simxr::app::windows::Error("Send test connector requests");
		Require(written == requests.size(), "Write complete test requests");
		const auto answered = [&] {
			return ReadLog(logPath).find(R"({"id":2,"jsonrpc":"2.0","result")") != std::string::npos;
		};
		const auto deadline = std::chrono::steady_clock::now() + 15s;
		DWORD pid = 0;
		while (std::chrono::steady_clock::now() < deadline)
		{
			pid = RecordProcess(directory);
			if ((pid && pid != handPid) || WaitForSingleObject(child.Get(), 0) == WAIT_OBJECT_0)
				break;
			if (expect == Expect::Attached && answered())
				break;
			std::this_thread::sleep_for(25ms);
		}
		if (expect == Expect::Attached)
		{
			if (pid != handPid || !answered())
				throw std::runtime_error("The connector must call through the hand-started app: " + ReadLog(logPath));
			Require(WaitForSingleObject(child.Get(), 0) == WAIT_TIMEOUT, "The attached connector keeps running");
			appProcess = std::move(handStarted);
		}
		else
		{
			if (!pid)
				throw std::runtime_error("MCP app did not start in " + simxr::app::PathText(directory.filename()) +
										 ": " + ReadLog(logPath));
			appProcess = Handle(OpenProcess(PROCESS_TERMINATE | SYNCHRONIZE, FALSE, pid));
			if (!appProcess.Get())
				throw simxr::app::windows::Error("Open test MCP app");
		}
		Require(WaitForSingleObject(appProcess.Get(), 0) == WAIT_TIMEOUT, "MCP app is alive");
		if (expect == Expect::Contained)
			Require(ReadLog(logPath).find("forbids breakaway") != std::string::npos,
					"A contained start must say the app closes with its client");
		job = Handle();
		Require(WaitForSingleObject(child.Get(), 10000) == WAIT_OBJECT_0, "Closing the job terminates the connector");
		if (expect == Expect::Contained)
		{
			Require(WaitForSingleObject(appProcess.Get(), 10000) == WAIT_OBJECT_0,
					"An app started inside the client's job closes with it");
			return;
		}
		Require(WaitForSingleObject(appProcess.Get(), 0) == WAIT_TIMEOUT, "MCP app survives its client's job shutdown");
		CheckWindows(TerminateProcess(appProcess.Get(), 1), "Terminate isolated test app");
		Require(WaitForSingleObject(appProcess.Get(), 10000) == WAIT_OBJECT_0, "Test app exits");
	}
	catch (...)
	{
		if (appProcess.Get())
		{
			TerminateProcess(appProcess.Get(), 1);
			WaitForSingleObject(appProcess.Get(), 10000);
		}
		if (handStarted.Get())
		{
			TerminateProcess(handStarted.Get(), 1);
			WaitForSingleObject(handStarted.Get(), 10000);
		}
		TerminateProcess(child.Get(), 1);
		WaitForSingleObject(child.Get(), 10000);
		throw;
	}
}

int wmain(int argc, wchar_t** argv)
{
	if (argc != 4)
		return 2;
	try
	{
		const std::filesystem::path connector(argv[1]), app(argv[2]), root(argv[3]);
		RunCase(connector, app, root / L"breakaway", JOB_OBJECT_LIMIT_BREAKAWAY_OK, Expect::Independent);
		RunCase(connector, app, root / L"silent-breakaway", JOB_OBJECT_LIMIT_SILENT_BREAKAWAY_OK, Expect::Independent);
		RunCase(connector, app, root / L"no-breakaway", 0, Expect::Contained);
		RunCase(connector, app, root / L"hand-started", 0, Expect::Attached);
		std::puts("PASS Windows job breakaway, silent breakaway, contained start and attach to a hand-started app");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "Windows MCP job test: %s\n", error.what());
		return 1;
	}
}
