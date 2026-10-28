#include "../TestSupport.h"
#include "app/services/UpdateChecker.h"
#ifdef _WIN32
#include "app/platform/windows/LaunchSupport.h"
#include "app/platform/windows/UpdateTransport.h"
#include <fcntl.h>
#include <io.h>
#endif

#include <chrono>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <thread>

using namespace simxr::app;
using namespace std::chrono_literals;

static void Reject(std::string_view repository, std::string_view current, std::string_view response)
{
	bool rejected = false;
	try
	{
		ParseRelease(repository, current, response);
	}
	catch (const std::exception&)
	{
		rejected = true;
	}
	Require(rejected, "Invalid release response was accepted");
}
static void CheckVersions()
{
	const auto newer = ParseRelease("valid-owner/repository", "2.0.0",
		R"({"tag_name":"v2.10.0","draft":false,"prerelease":false,"html_url":"https://evil.invalid"})");
	Require(newer.status == UpdateStatus::Available && newer.version == "v2.10.0" &&
			newer.releaseUrl == "https://github.com/valid-owner/repository/releases/tag/v2.10.0",
			"A newer release or its trusted URL was not identified");
	Require(ParseRelease("owner/repo", "2.10.0", R"({"tag_name":"v2.9.9","draft":false,"prerelease":false})").status ==
				UpdateStatus::Current, "Version comparison must use numeric components");
	Require(ParseRelease("owner/repo", "2.0.0", R"({"tag_name":"2.0.0","draft":false,"prerelease":false})").status ==
				UpdateStatus::Current, "An equal release must be current");
	Reject("owner/repo", "2.0.0", "not json");
	Reject("owner/repo", "2.0.0", R"({"tag_name":"v2.0.1-beta","draft":false,"prerelease":true})");
	Reject("owner/repo", "2.0.0", R"({"tag_name":"v2.0.1","draft":true,"prerelease":false})");
	Reject("owner/repo", "2.0.0", R"({"tag_name":"v2.00.1","draft":false,"prerelease":false})");
	Reject("owner/repo", "2.0.0", R"({"tag_name":"v99999999999999999.0.0","draft":false,"prerelease":false})");
	Reject("owner/repo", "2.0.0", R"({"tag_name":"v2.0.1"})");
	Reject("owner/../repo", "2.0.0", R"({"tag_name":"v2.0.1","draft":false,"prerelease":false})");
	Reject("owner/repo", "2.0.0", std::string(1024 * 1024 + 1, 'x'));
}
static void CheckWorker()
{
	UpdateChecker checker("owner/repo", "2.0.0", [](std::string_view)
	{
		std::this_thread::sleep_for(150ms);
		return std::string(R"({"tag_name":"v2.0.1","draft":false,"prerelease":false})");
	});
	checker.Start();
	Require(checker.State().status == UpdateStatus::Checking && !checker.Poll(), "A network check blocked the caller");
	Eventually([&] { return checker.Poll(); }, "Update worker did not finish");
	Require(checker.State().status == UpdateStatus::Available && !checker.Poll(), "Update result was not published once");
	UpdateChecker offline("owner/repo", "2.0.0", [](std::string_view) -> std::string
	{
		throw std::runtime_error("offline");
	});
	offline.Start();
	Eventually([&] { return offline.Poll(); }, "Offline update worker did not finish");
	Require(offline.State().status == UpdateStatus::Unavailable && offline.State().error == "offline",
			"Offline must remain unavailable");
	bool called = false;
	UpdateChecker unconfigured("", "2.0.0", [&](std::string_view) { called = true; return std::string{}; });
	unconfigured.Start();
	Require(!called && unconfigured.State().status == UpdateStatus::Unavailable,
			"Unconfigured builds must not contact a service");
}
int main(int argc, char** argv)
{
	try
	{
#ifdef _WIN32
		if (argc == 5 && std::string_view(argv[1]) == "--production-live")
		{
			const auto repository = std::string(argv[2]);
			const auto response = windows::RunUpdateWorker(L"--update-worker=" + windows::Wide(repository),
														windows::Wide(argv[3]));
			const auto result = ParseRelease(repository, "0.0.0", response);
			Require(result.status == UpdateStatus::Available, "Production update worker did not return a release");
			std::ofstream marker{std::filesystem::path(argv[4])};
			marker << result.version;
			marker.close();
			Require(static_cast<bool>(marker), "Cannot write production update result");
			return 0;
		}
		if (argc == 2 && std::string_view(argv[1]).starts_with("--update-worker="))
		{
			const auto result = windows::FetchLatestReleaseDirect(std::string_view(argv[1]).substr(16));
			_setmode(_fileno(stdout), _O_BINARY);
			std::cout.write(result.data(), static_cast<std::streamsize>(result.size()));
			return std::cout ? 0 : 1;
		}
		if (argc == 2 && std::string_view(argv[1]).starts_with("--slow-worker="))
		{
			const auto port = std::stoul(argv[1] + 14);
			windows::FetchUpdateResponse(L"127.0.0.1", static_cast<std::uint16_t>(port), L"/", false);
			return 1;
		}
		if (argc == 4 && std::string_view(argv[1]) == "--slow-http")
		{
			const auto port = std::stoul(argv[2]);
			Require(port > 0 && port <= 65535, "Invalid test port");
			const auto started = std::chrono::steady_clock::now();
			{
				UpdateChecker checker("test/repo", "2.0.0", [port](std::string_view)
				{
					return windows::RunUpdateWorker(L"--slow-worker=" + std::to_wstring(port));
				});
				checker.Start();
				Require(checker.State().status == UpdateStatus::Checking, "Slow request did not start");
			}
			const auto elapsed = std::chrono::steady_clock::now() - started;
			Require(elapsed < 12s, "Update worker blocked shutdown on slow headers");
			std::ofstream marker{std::filesystem::path(argv[3])};
			marker << "PASS";
			marker.close();
			Require(static_cast<bool>(marker), "Cannot write slow-header result");
			std::cout << "PASS slow-header update worker shutdown\n";
			return 0;
		}
#endif
		if (argc == 3 && std::string_view(argv[1]) == "--live")
		{
			UpdateChecker checker(argv[2], "0.0.0");
			checker.Start();
			const auto deadline = std::chrono::steady_clock::now() + 12s;
			while (!checker.Poll() && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(10ms);
			const auto& result = checker.State();
			if (result.status != UpdateStatus::Available)
				throw std::runtime_error("Live check unavailable: " + result.error);
			std::cout << "PASS HTTPS release check: " << result.version << '\n';
			return 0;
		}
		CheckVersions();
		CheckWorker();
		std::cout << "PASS update version parsing, asynchronous state and offline handling\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
