#include "Pch.h"
#include "app/mcp/McpApplication.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpInstance.h"
#include "app/mcp/McpServer.h"

int main(int argc, char** argv)
{
	std::intptr_t inherited = -1;
	if (argc == 3 && std::string_view(argv[1]) == "--mcp-bootstrap-handle")
	{
		const std::string_view number(argv[2]);
		const auto parsed = std::from_chars(number.data(), number.data() + number.size(), inherited);
		if (parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || inherited < 3)
			return 2;
	}
	else if (argc != 2 || std::string_view(argv[1]) != "--mcp")
		return 2;
	try
	{
		const auto token = simxr::app::TakeMcpToken();
		const auto credential = inherited < 0 ? simxr::app::NewMcpIdentifier() : token;
		simxr::app::McpInstanceRegistration registration(inherited);
		simxr::app::McpApplication application;
		simxr::app::McpServer server(application, 0, credential);
		registration.Publish({server.Port(), credential});
		std::this_thread::sleep_for(std::chrono::seconds(60));
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "MCP discovery fixture: %s\n", error.what());
		return 1;
	}
}
