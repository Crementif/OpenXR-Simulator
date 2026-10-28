#include "Pch.h"
#include "app/mcp/McpConnector.h"
#include "app/mcp/McpCredentials.h"
#include "app/mcp/McpConfiguration.h"
#include "simxr/Version.h"
#if defined(__linux__)
#include "platform/linux/InstallUse.h"
#endif
#ifdef SIMXR_MCP_DISCOVERY
#include "app/services/Utf8Path.h"
#endif

#include <charconv>
#include <cstdio>
#include <exception>
#include <string_view>
#ifdef _WIN32
#include "app/platform/windows/LaunchSupport.h"
#include "platform/windows/InstallUse.h"
#include <fcntl.h>
#include <io.h>
#endif

static int ConnectorMain(int argc, char** argv)
{
#ifdef _WIN32
	_setmode(_fileno(stdin), _O_BINARY);
	_setmode(_fileno(stdout), _O_BINARY);
#endif
	if (argc == 2 && std::string_view(argv[1]) == "--version")
	{
		if (std::printf("OpenXR Simulator MCP connector %s (connector protocol %u)\n",
			simxr::Version, static_cast<unsigned>(simxr::app::McpConnectorProtocolVersion)) < 0 || std::fflush(stdout) != 0)
			return 1;
		return 0;
	}
	if (argc == 2 && std::string_view(argv[1]) == "--help")
	{
#ifdef SIMXR_MCP_DISCOVERY
		std::fputs("Usage: simxr_mcp_connector [--app ABSOLUTE_PATH] [--windowless] [--print-config [--client generic|codex]] | --port PORT\n"
				   "--print-config writes JSON (generic) or TOML (codex) without starting the simulator.\n"
				   "--version prints the connector build and compatibility protocol.\n"
				   "The development --port mode reads its credential from SIMXR_MCP_TOKEN.\n", stderr);
#else
		std::fputs("Usage: simxr_mcp_connector --port PORT\n"
				   "The development --port mode reads its credential from SIMXR_MCP_TOKEN.\n", stderr);
#endif
		return 0;
	}
	try
	{
#ifdef SIMXR_MCP_DISCOVERY
		if (argc == 1 || std::string_view(argv[1]) != "--port")
		{
			bool windowless = false;
			bool printConfig = false;
			std::optional<simxr::app::McpConfigurationClient> client;
			std::optional<std::filesystem::path> selected;
			for (int index = 1; index < argc; ++index)
			{
				const std::string_view option(argv[index]);
				if (option == "--windowless" && !windowless)
					windowless = true;
				else if (option == "--print-config" && !printConfig)
					printConfig = true;
				else if (option == "--app" && !selected && index + 1 < argc)
					selected = simxr::app::Utf8Path(argv[++index]);
				else if (option == "--client" && !client && index + 1 < argc)
				{
					const std::string_view name(argv[++index]);
					if (name == "generic")
						client = simxr::app::McpConfigurationClient::Generic;
					else if (name == "codex")
						client = simxr::app::McpConfigurationClient::Codex;
					else
						throw std::invalid_argument("Unknown MCP configuration client; use generic or codex");
				}
				else
					throw std::invalid_argument("Invalid MCP connector options");
			}
			if (client && !printConfig)
				throw std::invalid_argument("--client requires --print-config");
			const auto executable = selected ? *selected : simxr::app::McpOwnExecutable().parent_path() /
#ifdef _WIN32
				"simxr_companion.exe";
#else
				"simxr_companion";
#endif
			if (!executable.is_absolute())
				throw std::invalid_argument("The selected MCP app path must be absolute");
			if (printConfig)
			{
				const auto output = simxr::app::McpConfiguration(simxr::app::McpOwnExecutable(), selected, windowless,
					client.value_or(simxr::app::McpConfigurationClient::Generic)) + '\n';
				if (std::fwrite(output.data(), 1, output.size(), stdout) != output.size() || std::fflush(stdout) != 0)
					throw std::runtime_error("Cannot write MCP configuration");
				return 0;
			}
			auto discovery = std::make_unique<simxr::app::McpInstanceDiscovery>(executable, windowless);
			discovery->StartIfNeeded();
			simxr::app::McpConnector(std::move(discovery)).Run();
			return 0;
		}
#endif
		if (argc != 3 || std::string_view(argv[1]) != "--port")
		{
			std::fputs("Invalid MCP connector options\n", stderr);
			return 2;
		}
		const std::string_view text(argv[2]);
		unsigned port = 0;
		const auto parsed = std::from_chars(text.data(), text.data() + text.size(), port);
		if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !port || port > 65535)
		{
			std::fputs("Invalid MCP endpoint port\n", stderr);
			return 2;
		}
		simxr::app::McpConnector(static_cast<std::uint16_t>(port), simxr::app::TakeMcpToken()).Run();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "MCP connector: %s\n", error.what());
		return 1;
	}
}

#ifdef _WIN32
int wmain(int argc, wchar_t** argv)
{
	simxr::windows::InstallUse installUse;
	if (!installUse.Ready())
	{
		std::fputs("OpenXR Simulator installation is being updated or removed. Try again.\n", stderr);
		return 1;
	}
	try
	{
		std::vector<std::string> values;
		values.reserve(argc);
		for (int index = 0; index < argc; ++index)
			values.push_back(simxr::app::windows::Utf8(argv[index]));
		std::vector<char*> arguments;
		arguments.reserve(values.size());
		for (auto& value : values)
			arguments.push_back(value.data());
		return ConnectorMain(argc, arguments.data());
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "MCP connector: %s\n", error.what());
		return 1;
	}
}
#else
int main(int argc, char** argv)
{
#if defined(__linux__)
	if (!simxr::InstallationReady())
	{
		std::fputs("OpenXR Simulator installation is being updated or is no longer available. Try again.\n", stderr);
		return 1;
	}
#endif
	return ConnectorMain(argc, argv);
}
#endif
