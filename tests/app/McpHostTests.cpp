#include "app/mcp/McpPch.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpHost.h"
#include "app/services/CompanionSettings.h"
#include "app/services/Utf8Path.h"
#include "../TestSupport.h"

#include <httplib.h>

int main(int argc, char** argv)
{
	if (argc == 3 && std::string_view(argv[1]) == "--read-settings")
	{
		std::puts(simxr::app::LoadSettings(simxr::app::Utf8Path(argv[2])).mcpEnabled ? "1" : "0");
		return 0;
	}
	if (argc == 4 && std::string_view(argv[1]) == "--write-settings")
	{
		simxr::app::CompanionSettings settings;
		settings.mcpEnabled = std::string_view(argv[3]) == "1";
		simxr::app::SaveSettings(simxr::app::Utf8Path(argv[2]), settings);
		return 0;
	}
	using namespace std::chrono_literals;
	using Json = nlohmann::json;
	const auto directory =
		std::filesystem::temp_directory_path() / ("simxr-mcp-host-" + simxr::app::NewMcpIdentifier());
#ifdef _WIN32
	Require(SetEnvironmentVariableW(L"SIMXR_MCP_DISCOVERY_DIR", directory.c_str()), "Set private MCP test directory");
#else
	Require(::setenv("SIMXR_MCP_DISCOVERY_DIR", directory.c_str(), 1) == 0, "Set private MCP test directory");
#endif
	try
	{
		simxr::app::McpHost host(false, std::uint16_t{0}, std::string(64, 'a'), -2);
		host.Update();
		auto state = host.State();
		Require(state.listening && state.startedForSession, "Explicit MCP startup does not alter the preference");
		const auto connection = Json::parse(state.httpConfiguration);
		const auto address = connection["url"].get<std::string>();
		const auto portStart = address.find(':', std::string("http://").size());
		const auto portEnd = address.find('/', portStart);
		const auto port = std::stoi(address.substr(portStart + 1, portEnd - portStart - 1));
		httplib::Client client("127.0.0.1", port);
		const httplib::Headers headers{{"Authorization", "Bearer " + std::string(64, 'a')},
									   {"Accept", "application/json, text/event-stream"}};
		const Json initialize = {{"jsonrpc", "2.0"},
								 {"id", 1},
								 {"method", "initialize"},
								 {"params",
								  {{"protocolVersion", "2025-11-25"},
								   {"capabilities", Json::object()},
								   {"clientInfo", {{"name", "host-test"}, {"version", "1"}}}}}};
		const auto initialized = client.Post("/mcp", headers, initialize.dump(), "application/json");
		Require(initialized && initialized->status == 200, "Initialize host MCP session");
		auto sessionHeaders = headers;
		sessionHeaders.emplace("MCP-Session-Id", initialized->get_header_value("MCP-Session-Id"));
		const auto notification =
			client.Post("/mcp", sessionHeaders,
						Json({{"jsonrpc", "2.0"}, {"method", "notifications/initialized"}}).dump(), "application/json");
		Require(notification && notification->status == 202 && host.State().sessions == 1,
				"Host reports initialized client sessions");
		const auto listed =
			client.Post("/mcp", sessionHeaders, Json({{"jsonrpc", "2.0"}, {"id", 2}, {"method", "tools/list"}}).dump(),
						"application/json");
		Require(listed && listed->status == 200, "List MCP tools");
		const auto tools = Json::parse(listed->body).at("result").at("tools");
		const auto hasLuau =
			std::ranges::any_of(tools, [](const auto& tool) { return tool.value("name", "") == "simulator_luau_run"; });
#ifdef SIMXR_COMPANION_AUTOMATION
		Require(hasLuau, "Automation build did not advertise Luau");
#else
		Require(!hasLuau, "Build without automation advertised Luau");
#endif
		const auto before = std::chrono::steady_clock::now();
		host.StopForSession();
		host.Update();
		Require(std::chrono::steady_clock::now() - before < 500ms, "Stopping MCP must not wait on the UI thread");
		Eventually(
			[&] {
				host.Update();
				return !host.State().stopping;
			},
			"MCP service did not stop");
		Require(!host.State().listening, "MCP service stops for this session");
		host.SetPreference(true);
		host.Update();
		Require(host.State().listening && !host.State().startedForSession, "Saved preference starts discoverable MCP");
		host.SetPreference(false);
		host.Update();
		Eventually(
			[&] {
				host.Update();
				return !host.State().stopping;
			},
			"Preferred MCP service did not stop");
		Require(!host.State().listening, "Disabling the preference stops MCP");
	}
	catch (...)
	{
		std::error_code ignored;
		std::filesystem::remove_all(directory, ignored);
		throw;
	}
	std::error_code ignored;
	std::filesystem::remove_all(directory, ignored);
	return 0;
}
