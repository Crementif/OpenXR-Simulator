#include "app/mcp/McpPch.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpDebug.h"
#include "app/mcp/McpDiagnostics.h"
#include "app/mcp/McpLogs.h"
#include "app/mcp/McpServer.h"
#include "app/mcp/McpScreenshot.h"
#include "app/services/ApplicationCatalog.h"
#include "../TestSupport.h"

#include <future>
#include <iterator>

#ifdef _WIN32
#include "app/platform/windows/LaunchSupport.h"
#include "app/platform/windows/ManifestAlias.h"
#include "app/services/Utf8Path.h"
#endif
#ifdef __linux__
#include "../control/FakeImageInspection.h"
#include "control/LocalControlServer.h"
#include <unistd.h>
#endif

using Json = nlohmann::json;

class EmptyCatalog final : public simxr::app::ApplicationCatalog
{
  public:
	std::uint64_t LaunchApplication(const simxr::app::LaunchProfile&) override
	{
		throw std::logic_error("Unexpected launch");
	}
	void RequestApplicationClose(std::uint64_t) override
	{
		throw std::logic_error("Unexpected close");
	}
	void RequestApplicationRestart(std::uint64_t, std::uint64_t) override
	{
		throw std::logic_error("Unexpected restart");
	}
	std::vector<simxr::app::ApplicationSnapshot> Applications() const override
	{
		return {};
	}
	bool RequestShutdown() override
	{
		return false;
	}
};

class ArtifactCatalog final : public simxr::app::ApplicationCatalog
{
  public:
	simxr::app::ApplicationSnapshot snapshot;
	bool changeAfterFirst = false;
	mutable std::atomic<unsigned> reads{0};
	std::uint64_t LaunchApplication(const simxr::app::LaunchProfile&) override { throw std::logic_error("Unexpected launch"); }
	void RequestApplicationClose(std::uint64_t) override { throw std::logic_error("Unexpected close"); }
	void RequestApplicationRestart(std::uint64_t, std::uint64_t) override { throw std::logic_error("Unexpected restart"); }
	std::vector<simxr::app::ApplicationSnapshot> Applications() const override
	{
		auto value = snapshot;
		if (reads.fetch_add(1) > 0 && changeAfterFirst)
		{
			value.session = 2;
			value.generation = 2;
			value.runtimeEpoch = 2;
		}
		return {std::move(value)};
	}
	bool RequestShutdown() override { return false; }
};

static void ApplicationLookups()
{
	using namespace std::chrono_literals;
	ArtifactCatalog catalog;
	catalog.snapshot.id = UINT64_MAX;
	catalog.snapshot.runtimeEpoch = UINT64_MAX - 1;
	catalog.snapshot.launchDataDirectory = "/tmp/runtime";
	simxr::app::McpApplication application;
	const auto id = application.ApplicationDocument(catalog.snapshot).at("application_id").get<std::string>();
	auto lookup = std::async(std::launch::async, [&]
	{
		return application.Lookup(id, std::make_shared<simxr::app::McpCancellation>());
	});
	Eventually([&] { application.Poll(catalog); return lookup.wait_for(0ms) == std::future_status::ready; }, "Typed lookup completes");
	const auto snapshot = lookup.get();
	Require(snapshot && snapshot->id == UINT64_MAX && snapshot->runtimeEpoch == UINT64_MAX - 1 &&
		snapshot->launchDataDirectory == catalog.snapshot.launchDataDirectory, "Typed lookup preserves snapshot values");
	bool foreign = false;
	try { application.Lookup("different-instance:1", std::make_shared<simxr::app::McpCancellation>()); }
	catch (const std::invalid_argument&) { foreign = true; }
	Require(foreign, "Typed lookup rejects another simulator instance");
	catalog.snapshot.id = 1;
	auto missing = std::async(std::launch::async, [&]
	{
		try { application.Lookup(id, std::make_shared<simxr::app::McpCancellation>()); }
		catch (const std::runtime_error&) { return true; }
		return false;
	});
	Eventually([&] { application.Poll(catalog); return missing.wait_for(0ms) == std::future_status::ready; }, "Removed lookup completes");
	Require(missing.get(), "Typed lookup rejects a removed application");
	const auto reads = catalog.reads.load();
	auto cancellation = std::make_shared<simxr::app::McpCancellation>();
	auto cancelled = std::async(std::launch::async, [&] { return application.Lookup(id, cancellation); });
	Require(cancelled.wait_for(100ms) == std::future_status::timeout, "Typed lookup waits for the application thread");
	cancellation->Cancel();
	Require(cancelled.wait_for(500ms) == std::future_status::ready && !cancelled.get(), "Queued lookup cancellation wakes its caller");
	application.Poll(catalog);
	Require(catalog.reads == reads, "Cancelled lookup does not access the catalog");
	auto closing = std::async(std::launch::async, [&]
	{
		try { application.Lookup(id, std::make_shared<simxr::app::McpCancellation>()); }
		catch (const std::runtime_error& error) { return std::string(error.what()).find("shutting down") != std::string::npos; }
		return false;
	});
	Require(closing.wait_for(100ms) == std::future_status::timeout, "Shutdown lookup is queued");
	application.Close();
	Require(closing.wait_for(500ms) == std::future_status::ready && closing.get(), "Close wakes a pending typed lookup");
}

#ifdef __linux__
static void ArtifactLookupCancellation()
{
	using namespace std::chrono_literals;
	std::array<char, 64> directory{};
	std::strcpy(directory.data(), "/tmp/simxr-artifact-cancel-XXXXXX");
	Require(::mkdtemp(directory.data()) != nullptr, "Create artifact cancellation directory");
	struct RemoveDirectory
	{
		std::filesystem::path path;
		~RemoveDirectory() { std::filesystem::remove_all(path); }
	} cleanup{directory.data()};
	simxr::control::test::Fixture fixture;
	simxr::control::LocalControlServer server(fixture.service, (cleanup.path / "control.sock").string());
	ArtifactCatalog catalog;
	catalog.snapshot.id = 1;
	catalog.snapshot.session = 1;
	catalog.snapshot.generation = 1;
	catalog.snapshot.runtimeProcess = static_cast<std::uint64_t>(::getpid());
	catalog.snapshot.runtimeEpoch = 1;
	catalog.snapshot.connected = catalog.snapshot.running = true;
	catalog.snapshot.endpoint = server.Path();
	simxr::app::McpApplication application;
	auto list = std::async(std::launch::async, [&]
	{
		return application.Call("simulator_list_applications", Json::object(), std::make_shared<simxr::app::McpCancellation>());
	});
	Eventually([&] { application.Poll(catalog); return list.wait_for(0ms) == std::future_status::ready; }, "List fixture application");
	const auto listing = list.get();
	const auto app = Json::parse(listing->at("content").at(0).at("text").get<std::string>()).at("applications").at(0).at("application_id");
	{
		simxr::app::McpDebug bounded;
		const auto open = [&]() -> std::optional<simxr::app::McpConnection>
		{
			auto pending = std::async(std::launch::async, [&]
			{
				try { return bounded.Connect(application, app.get<std::string>(), std::make_shared<simxr::app::McpCancellation>()); }
				catch (const std::runtime_error&) { return std::optional<simxr::app::McpConnection>{}; }
			});
			Eventually([&] { application.Poll(catalog); return pending.wait_for(0ms) == std::future_status::ready; }, "Typed connection completes");
			return pending.get();
		};
		++catalog.snapshot.runtimeProcess;
		for (unsigned index = 0; index < 9; ++index)
			Require(!open(), "Typed connection rejects the wrong runtime process");
		--catalog.snapshot.runtimeProcess;
		std::vector<std::string> connections;
		for (unsigned index = 0; index < 8; ++index)
		{
			const auto opened = open();
			Require(opened && !opened->id.empty() && opened->process == catalog.snapshot.runtimeProcess &&
				opened->epoch == 1 && opened->session == 1 && opened->generation == 1,
				"Failed connections release capacity and successful connections retain target metadata");
			connections.push_back(opened->id);
		}
		Require(!open(), "Typed connections enforce the per-session capacity");
		bounded.Disconnect(connections.front());
		Require(open().has_value(), "Disconnect restores connection capacity");
	}

	simxr::app::McpDebug debug;
	auto connect = std::async(std::launch::async, [&]
	{
		return debug.Call(application, "debug_connect", {{"application_id", app}}, std::make_shared<simxr::app::McpCancellation>());
	});
	Eventually([&] { application.Poll(catalog); return connect.wait_for(0ms) == std::future_status::ready; }, "Connect fixture debugger");
	const auto opened = connect.get();
	Require(opened && !opened->at("isError").get<bool>(), "Fixture debugger connected");
	const auto connection = Json::parse(opened->at("content").at(0).at("text").get<std::string>()).at("connection").get<std::string>();
	bool rejectedScope = false;
	try
	{
		using namespace simxr::control;
		debug.Request(application, connection, OpCode::ExportInfo,
			{Field::U64(tag::ExportId, 1), Field::U64(tag::Session, 1), Field::U64(tag::Generation, 2)});
	}
	catch (const std::invalid_argument&)
	{
		rejectedScope = true;
	}
	Require(rejectedScope, "Typed artifact request accepted a different generation");
	auto unavailable = std::async(std::launch::async, [&]
	{
		using namespace simxr::control;
		return debug.Request(application, connection, OpCode::ExportInfo,
			{Field::U64(tag::ExportId, UINT64_MAX)});
	});
	Eventually([&] { application.Poll(catalog); return unavailable.wait_for(0ms) == std::future_status::ready; },
		"Typed artifact lookup did not finish");
	Require(unavailable.get().Find(simxr::control::tag::Error)->AsU32() != 0,
		"Typed artifact lookup hid the runtime rejection");
	auto cancellation = std::make_shared<simxr::app::McpCancellation>();
	auto read = std::async(std::launch::async, [&]
	{
		try { (void)debug.ReadArtifact(application, connection, 1, 0, 1, cancellation); }
		catch (const std::exception&) { return true; }
		return false;
	});
	Require(read.wait_for(100ms) == std::future_status::timeout, "Artifact read waits on application status");
	cancellation->Cancel();
	Require(read.wait_for(500ms) == std::future_status::ready && read.get(), "Cancellation interrupts artifact status lookup");
	application.Poll(catalog);
	auto reconnect = std::async(std::launch::async, [&]
	{
		return debug.Call(application, "debug_connect", {{"application_id", app}}, std::make_shared<simxr::app::McpCancellation>());
	});
	Eventually([&] { application.Poll(catalog); return reconnect.wait_for(0ms) == std::future_status::ready; }, "Reconnect fixture debugger");
	const auto reconnected = reconnect.get();
	Require(reconnected && !reconnected->at("isError").get<bool>(), "Fixture debugger reconnected");
	const auto second = Json::parse(reconnected->at("content").at(0).at("text").get<std::string>()).at("connection").get<std::string>();
	const auto destination = cleanup.path / "cancelled.mp4";
	auto stopSave = std::make_shared<simxr::app::McpCancellation>();
	auto saving = std::async(std::launch::async, [&]
	{
		return debug.Call(application, "debug_save_video", {{"connection", second}, {"first_frame", "1"},
			{"last_frame", "1"}, {"path", destination.string()}, {"timeout", 5}}, stopSave);
	});
	Require(saving.wait_for(100ms) == std::future_status::timeout, "Video save waits on application status");
	stopSave->Cancel();
	Require(saving.wait_for(500ms) == std::future_status::ready && !saving.get(), "Video cancellation completes promptly");
	Require(!std::filesystem::exists(destination), "Cancelled video did not publish a file");
	const auto closed = debug.Call(application, "debug_status", {{"connection", second}}, std::make_shared<simxr::app::McpCancellation>());
	Require(closed && closed->at("isError").get<bool>(), "Cancelled video retired its debugger connection");
	application.Poll(catalog);
	auto imageConnect = std::async(std::launch::async, [&]
	{
		return debug.Call(application, "debug_connect", {{"application_id", app}}, std::make_shared<simxr::app::McpCancellation>());
	});
	Eventually([&] { application.Poll(catalog); return imageConnect.wait_for(0ms) == std::future_status::ready; },
		"Reconnect image fixture debugger");
	const auto imageOpened = imageConnect.get();
	Require(imageOpened && !imageOpened->at("isError").get<bool>(), "Image fixture debugger reconnected");
	const auto imageConnection = Json::parse(imageOpened->at("content").at(0).at("text").get<std::string>())
		.at("connection").get<std::string>();
	const auto imagePath = cleanup.path / "protected.png";
	{
		std::ofstream existing(imagePath, std::ios::binary);
		existing << "preserved";
	}
	auto stopImage = std::make_shared<simxr::app::McpCancellation>();
	auto imageSave = std::async(std::launch::async, [&]
	{
		return debug.Call(application, "debug_save_image", {{"connection", imageConnection}, {"export", "1"},
			{"path", imagePath.string()}, {"timeout", 5}, {"overwrite", true}}, stopImage);
	});
	Require(imageSave.wait_for(100ms) == std::future_status::timeout, "Image save waits on application status");
	stopImage->Cancel();
	Require(imageSave.wait_for(500ms) == std::future_status::ready && !imageSave.get(),
			"Image cancellation completes promptly");
	std::ifstream preserved(imagePath, std::ios::binary);
	const std::string contents((std::istreambuf_iterator<char>(preserved)), {});
	Require(contents == "preserved", "Cancelled image did not replace the existing file");
	const auto imageClosed = debug.Call(application, "debug_status", {{"connection", imageConnection}},
		std::make_shared<simxr::app::McpCancellation>());
	Require(imageClosed && imageClosed->at("isError").get<bool>(), "Cancelled image retired its debugger connection");
	application.Poll(catalog);
	auto sequenceConnect = std::async(std::launch::async, [&]
	{
		return debug.Call(application, "debug_connect", {{"application_id", app}}, std::make_shared<simxr::app::McpCancellation>());
	});
	Eventually([&] { application.Poll(catalog); return sequenceConnect.wait_for(0ms) == std::future_status::ready; },
		"Reconnect sequence fixture debugger");
	const auto sequenceOpened = sequenceConnect.get();
	Require(sequenceOpened && !sequenceOpened->at("isError").get<bool>(), "Sequence fixture debugger reconnected");
	const auto sequenceConnection = Json::parse(sequenceOpened->at("content").at(0).at("text").get<std::string>())
		.at("connection").get<std::string>();
	auto stopSequence = std::make_shared<simxr::app::McpCancellation>();
	auto sequenceSave = std::async(std::launch::async, [&]
	{
		return debug.Call(application, "capture_pose_sequence", {{"connection", sequenceConnection}, {"program_id", "1"},
			{"program", "{\"frame\":0,\"capture\":true}\n"}, {"directory", cleanup.path.string()}, {"timeout", 5}}, stopSequence);
	});
	Require(sequenceSave.wait_for(100ms) == std::future_status::timeout, "Sequence capture waits on application status");
	stopSequence->Cancel();
	Require(sequenceSave.wait_for(500ms) == std::future_status::ready && !sequenceSave.get(),
			"Sequence cancellation completes promptly");
	std::size_t manifests = 0;
	for (const auto& item : std::filesystem::directory_iterator(cleanup.path))
	{
		if (!item.is_directory() || !item.path().filename().string().starts_with("sequence-"))
			continue;
		std::ifstream stream(item.path() / "manifest.json", std::ios::binary);
		const auto document = Json::parse(stream);
		Require(document.at("phase") == "cancelled" && document.at("incomplete") == true,
				"Cancelled sequence manifest reports incomplete work");
		++manifests;
	}
	Require(manifests == 1, "Cancelled sequence retained one manifest");
	const auto sequenceClosed = debug.Call(application, "debug_status", {{"connection", sequenceConnection}},
		std::make_shared<simxr::app::McpCancellation>());
	Require(sequenceClosed && sequenceClosed->at("isError").get<bool>(), "Cancelled sequence retired its debugger connection");
	application.Poll(catalog);
	catalog.changeAfterFirst = true;
	catalog.reads = 0;
	auto replaced = std::async(std::launch::async, [&]
	{
		return simxr::app::mcp::RunDiagnostics(application, "get_diagnostics", {{"application_id", app}},
			std::make_shared<simxr::app::McpCancellation>());
	});
	Eventually([&] { application.Poll(catalog); return replaced.wait_for(0ms) == std::future_status::ready; },
		"Diagnostic replacement test completes");
	const auto mixed = replaced.get();
	Require(mixed && mixed->at("isError").get<bool>() && mixed->dump().find("changed during diagnostic collection") != std::string::npos,
		"Diagnostics reject a session replacement between status and connection");
	catalog.reads = 0;
	auto logs = std::async(std::launch::async, [&]
	{
		return simxr::app::mcp::LogCall(application, {{"application_id", app}, {"directory", cleanup.path.string()}}, false,
			std::make_shared<simxr::app::McpCancellation>());
	});
	Eventually([&] { application.Poll(catalog); return logs.wait_for(0ms) == std::future_status::ready; },
		"Log replacement test completes");
	const auto changedLogs = logs.get();
	Require(changedLogs && changedLogs->at("isError").get<bool>() && changedLogs->dump().find("runtime changed while reading logs") != std::string::npos,
		"Log reads reject runtime replacement across the file read");
	const auto logPath = cleanup.path / ("openxr_simulator." + std::to_string(::getpid()) + ".log");
	std::ofstream(logPath) << "preserve";
	catalog.reads = 0;
	auto clear = std::async(std::launch::async, [&]
	{
		return simxr::app::mcp::LogCall(application, {{"application_id", app}, {"directory", cleanup.path.string()}}, true,
			std::make_shared<simxr::app::McpCancellation>());
	});
	Eventually([&] { application.Poll(catalog); return clear.wait_for(0ms) == std::future_status::ready; },
		"Log clear replacement test completes");
	const auto changedClear = clear.get();
	Require(changedClear && changedClear->at("isError").get<bool>() &&
		changedClear->dump().find("runtime changed before clearing logs") != std::string::npos &&
		std::filesystem::file_size(logPath) == 8, "Log clear must not mutate a replaced runtime's file");
	auto cancelled = std::make_shared<simxr::app::McpCancellation>();
	cancelled->Cancel();
	Require(!simxr::app::mcp::LogCall(application, {{"application_id", app}, {"directory", cleanup.path.string()}}, true, cancelled) &&
		std::filesystem::file_size(logPath) == 8, "Pre-admission cancellation must not clear the log");
	application.Close();
}
#endif

struct HttpReply
{
	int status;
	std::string body, session;
};

static HttpReply Post(std::uint16_t port, const std::string& session, const Json& message,
					  const std::string& cancellationKey = {})
{
	httplib::Client client("127.0.0.1", port);
	client.set_read_timeout(std::chrono::seconds(8));
	httplib::Headers headers{{"Authorization", "Bearer " + std::string(64, 'a')},
							 {"Accept", "application/json, text/event-stream"}};
	if (!session.empty())
		headers.emplace("MCP-Session-Id", session);
	if (!cancellationKey.empty())
		headers.emplace("X-Simxr-Cancellation-Key", cancellationKey);
	const auto result = client.Post("/mcp", headers, message.dump(), "application/json");
	Require(static_cast<bool>(result), "HTTP request completed");
	return {result->status, result->body, result->get_header_value("MCP-Session-Id")};
}

static std::string Initialize(std::uint16_t port)
{
	const auto response = Post(port, {}, {{"jsonrpc", "2.0"}, {"id", 1}, {"method", "initialize"},
		{"params", {{"protocolVersion", "2025-11-25"}, {"capabilities", Json::object()},
					{"clientInfo", {{"name", "test"}, {"version", "1"}}}}}});
	Require(response.status == 200 && !response.session.empty(), "Initialize creates a session");
	Require(Post(port, response.session, {{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).status == 202,
			"Initialize notification accepted");
	return response.session;
}

static std::future<HttpReply> StartCall(std::uint16_t port, const std::string& session)
{
	return std::async(std::launch::async, [port, session]
	{
		return Post(port, session, {{"jsonrpc", "2.0"}, {"id", 17}, {"method", "tools/call"},
			{"params", {{"name", "simulator_launch_application"},
						{"arguments", {{"executable", "/unused"}}}}}});
	});
}

static void ExpectDuplicate(std::future<HttpReply>& first, std::future<HttpReply>& second)
{
	using namespace std::chrono_literals;
	Eventually([&] { return first.wait_for(0ms) == std::future_status::ready ||
							 second.wait_for(0ms) == std::future_status::ready; }, "Duplicate request rejected");
	auto& duplicate = first.wait_for(0ms) == std::future_status::ready ? first : second;
	const auto response = duplicate.get();
	Require(response.status == 200 && Json::parse(response.body).contains("error"),
			"Duplicate request ID is rejected within the session");
}

static void Cancel(std::uint16_t port, const std::string& session, const std::string& cancellationKey = {})
{
	const auto response = Post(port, session, {{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"},
		{"params", {{"requestId", 17}}}}, cancellationKey);
	Require(response.status == 202 && response.body.empty(), "Cancellation notification accepted");
}

static void DeleteSession(std::uint16_t port, const std::string& session)
{
	httplib::Client client("127.0.0.1", port);
	const httplib::Headers headers{{"Authorization", "Bearer " + std::string(64, 'a')},
								   {"MCP-Session-Id", session}};
	const auto result = client.Delete("/mcp", headers);
	Require(static_cast<bool>(result) && result->status == 204, "Session shutdown accepted");
}

static void ExpectCancelled(std::future<HttpReply>& first, std::future<HttpReply>& second)
{
	using namespace std::chrono_literals;
	auto& remaining = first.valid() ? first : second;
	Require(remaining.wait_for(1s) == std::future_status::ready, "Cancelled HTTP request finishes promptly");
	const auto result = remaining.get();
	Require(result.status == 202 && result.body.empty(), "Cancelled request has no JSON-RPC response");
}

int main()
{
	using namespace std::chrono_literals;
	ApplicationLookups();
#ifdef _WIN32
	{
		const auto original = std::filesystem::current_path();
		const auto root = std::filesystem::temp_directory_path() /
			("simxr-manifest-path-" + simxr::app::NewMcpIdentifier());
		struct Cleanup
		{
			std::filesystem::path original, root;
			~Cleanup()
			{
				std::error_code ignored;
				std::filesystem::current_path(original, ignored);
				std::filesystem::remove_all(root, ignored);
			}
		} cleanup{original, root};
		const auto relative = std::filesystem::path(L"runtime Ω") / L"openxr_simulator.json";
		const auto owner = root / L"companion", child = root / L"child";
		std::filesystem::create_directories((owner / relative).parent_path());
		std::filesystem::create_directories((child / relative).parent_path());
		std::ofstream(owner / relative) << "owner";
		std::ofstream(child / relative) << "child";
		std::filesystem::current_path(owner);
		const auto selected = simxr::app::windows::LoaderManifestPath(relative.native());
		Require(selected == relative.native(), "A relative manifest must retain the child's working-directory semantics");
		const auto absolute = owner / relative;
		const auto alias = simxr::app::windows::LoaderManifestPath(absolute.native());
		Require(std::filesystem::equivalent(absolute, std::filesystem::path(alias)), "A manifest alias must identify the same file");
		std::ofstream(absolute, std::ios::binary | std::ios::trunc)
			<< R"({"file_format_version":"1.0.0","runtime":{"library_path":"./runtime.dll","custom":"kept"}})";
		const auto cached = simxr::app::windows::PrepareManifestAlias(absolute.native());
		Require(cached != absolute.native(), "Unicode manifest must have an ASCII compatibility copy");
		Require(std::ranges::all_of(cached, [](wchar_t character) { return character <= 0x7f; }),
			"Compatibility manifest path must be ASCII");
		std::ifstream cachedFile{std::filesystem::path(cached), std::ios::binary};
		const auto cachedJson = Json::parse(cachedFile);
		Require(cachedJson["runtime"]["custom"] == "kept", "Custom manifest fields must survive");
		Require(cachedJson["runtime"]["library_path"] == simxr::app::PathText(absolute.parent_path() / L"./runtime.dll"),
			"Compatibility manifest must use the original runtime location");
		Require(simxr::app::windows::PrepareManifestAlias(absolute.native()) == cached,
			"The same manifest must reuse its immutable compatibility copy");
	}
#endif
#ifdef __linux__
	ArtifactLookupCancellation();
#endif
	simxr::app::McpApplication application;
	auto cancellation = std::make_shared<simxr::app::McpCancellation>();
	auto call = std::async(std::launch::async, [&]
	{
		return application.Call("simulator_launch_application", {{"executable", "/unused"}}, cancellation);
	});
	Require(call.wait_for(100ms) == std::future_status::timeout, "Request waits for application-thread admission");
	cancellation->Cancel();
	Require(call.wait_for(1s) == std::future_status::ready, "Cancellation wakes the waiting transport");
	Require(!call.get(), "Cancellation prevents a queued mutation from producing a result");
	application.Close();
	simxr::app::McpApplication preflight;
	EmptyCatalog catalog;
	simxr::app::McpApplication delayed;
	const auto delayedId = delayed.ApplicationDocument(simxr::app::ApplicationSnapshot{.id = 1}).at("application_id");
	for (unsigned attempt = 0; attempt < 16; ++attempt)
	{
		auto timeout = std::make_shared<simxr::app::McpCancellation>();
		auto screenshot = std::async(std::launch::async, [&]
		{
			return simxr::app::mcp::CaptureScreenshot(delayed, {{"application_id", delayedId}, {"timeout", .02}}, timeout);
		});
		Require(screenshot.wait_for(500ms) == std::future_status::ready, "Screenshot deadline interrupts queued application lookup");
		const auto result = screenshot.get();
		Require(result && result->at("isError").get<bool>() && result->dump().find("timed out") != std::string::npos,
			"Unadmitted screenshot reports its deadline");
		delayed.Poll(catalog);
	}
	{
		auto cancel = std::make_shared<simxr::app::McpCancellation>();
		auto screenshot = std::async(std::launch::async, [&]
		{
			return simxr::app::mcp::CaptureScreenshot(delayed, {{"application_id", delayedId}, {"timeout", 5}}, cancel);
		});
		std::this_thread::sleep_for(20ms);
		cancel->Cancel();
		Require(screenshot.wait_for(500ms) == std::future_status::ready && !screenshot.get(),
			"Cancellation interrupts queued screenshot lookup without a response");
		delayed.Poll(catalog);
	}
	{
		auto cancel = std::make_shared<simxr::app::McpCancellation>();
		auto stereo = std::async(std::launch::async, [&]
		{
			return simxr::app::mcp::ValidateStereo(delayed, {{"application_id", delayedId}, {"timeout", 5}}, cancel);
		});
		std::this_thread::sleep_for(20ms);
		cancel->Cancel();
		Require(stereo.wait_for(500ms) == std::future_status::ready && !stereo.get(),
			"Cancellation interrupts queued stereo lookup without a response");
		delayed.Poll(catalog);
	}
	{
		auto cancel = std::make_shared<simxr::app::McpCancellation>();
		auto diagnostics = std::async(std::launch::async, [&]
		{
			return simxr::app::mcp::RunDiagnostics(delayed, "get_diagnostics", {{"application_id", delayedId}}, cancel);
		});
		std::this_thread::sleep_for(20ms);
		cancel->Cancel();
		Require(diagnostics.wait_for(500ms) == std::future_status::ready && !diagnostics.get(),
			"Cancellation interrupts queued diagnostic lookup without a response");
		delayed.Poll(catalog);
	}
	delayed.Close();
	auto continuation = std::make_shared<simxr::app::McpCancellation>();
	auto lookup = std::async(std::launch::async, [&]
	{
		return preflight.Call("simulator_list_applications", Json::object(), continuation);
	});
	Eventually([&]
	{
		preflight.Poll(catalog);
		return lookup.wait_for(0ms) == std::future_status::ready;
	}, "Preflight request did not complete");
	Require(lookup.get().has_value(), "Preflight should return its result");
	continuation->Cancel();
	Require(continuation->Requested() && !continuation->TryAdmit(),
			"Cancellation after preflight must prevent a later runtime mutation");
#ifndef SIMXR_COMPANION_AUTOMATION
	auto unsupported = std::async(std::launch::async, [&]
	{
		return preflight.Call("simulator_luau_run", {{"application_id", "1"}, {"source", "print('test')"}},
							  std::make_shared<simxr::app::McpCancellation>());
	});
	Eventually([&]
	{
		preflight.Poll(catalog);
		return unsupported.wait_for(0ms) == std::future_status::ready;
	}, "Unsupported Luau request did not complete");
	const auto unsupportedResult = unsupported.get();
	Require(unsupportedResult && unsupportedResult->at("isError").get<bool>() &&
				unsupportedResult->dump().find("Unknown tool") != std::string::npos,
			"Automation-disabled build must reject a direct Luau request");
#endif

	simxr::app::McpApplication sharedApplication;
	simxr::app::McpServer server(sharedApplication, 0, std::string(64, 'a'));
	bool duplicateRejected = false;
	try
	{
		simxr::app::McpApplication otherApplication;
		simxr::app::McpServer duplicate(otherApplication, server.Port(), std::string(64, 'b'));
	}
	catch (const std::exception&)
	{
		duplicateRejected = true;
	}
	Require(duplicateRejected, "A second live MCP listener cannot bind the same endpoint");
	const auto firstSession = Initialize(server.Port());
	const auto secondSession = Initialize(server.Port());
	Require(firstSession != secondSession, "Separate clients receive separate sessions");
	const std::string earlyKey(64, 'c');
	Cancel(server.Port(), firstSession, earlyKey);
	const auto early = Post(server.Port(), firstSession,
		{{"jsonrpc", "2.0"}, {"id", 17}, {"method", "tools/call"},
		 {"params", {{"name", "simulator_launch_application"}, {"arguments", {{"executable", "/unused"}}}}}},
		earlyKey);
	Require(early.status == 202 && early.body.empty(), "Cancellation arriving before a keyed call prevents admission");
	auto first = StartCall(server.Port(), firstSession);
	auto firstDuplicate = StartCall(server.Port(), firstSession);
	auto second = StartCall(server.Port(), secondSession);
	auto secondDuplicate = StartCall(server.Port(), secondSession);
	ExpectDuplicate(first, firstDuplicate);
	ExpectDuplicate(second, secondDuplicate);
	Cancel(server.Port(), firstSession);
	ExpectCancelled(first, firstDuplicate);
	auto& other = second.valid() ? second : secondDuplicate;
	Require(other.wait_for(100ms) == std::future_status::timeout,
			"Cancellation does not affect the same request ID in another session");
	DeleteSession(server.Port(), secondSession);
	ExpectCancelled(second, secondDuplicate);
	const auto limitSession = Initialize(server.Port());
	for (unsigned index = 0; index < 64; ++index)
	{
		const auto digits = std::to_string(index);
		Cancel(server.Port(), limitSession, std::string(64 - digits.size(), '0') + digits);
	}
	const std::string overflowKey(64, 'd');
	const auto overflow = Post(server.Port(), limitSession,
		{{"jsonrpc", "2.0"}, {"method", "notifications/cancelled"}, {"params", {{"requestId", 17}}}}, overflowKey);
	Require(overflow.status == 503, "Early-cancellation capacity is bounded");
	const auto afterOverflow = Post(server.Port(), limitSession,
		{{"jsonrpc", "2.0"}, {"id", 17}, {"method", "tools/call"},
		 {"params", {{"name", "simulator_launch_application"}, {"arguments", {{"executable", "/unused"}}}}}},
		overflowKey);
	Require(afterOverflow.status == 404, "Cancellation overflow retires the session before admitting the call");
	server.Stop();
	return 0;
}
