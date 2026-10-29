#include "app/mcp/McpPch.h"
#include "app/mcp/McpConfiguration.h"
#include "app/services/Utf8Path.h"

namespace simxr::app
{
std::string McpConfiguration(const std::filesystem::path& connector, const std::optional<std::filesystem::path>& app,
							 bool windowless, McpConfigurationClient client)
{
	using Json = nlohmann::json;
	if (!connector.is_absolute() || (app && !app->is_absolute()))
		throw std::invalid_argument("MCP connector and app paths must be absolute");
	Json args = Json::array();
	if (app)
	{
		args.push_back("--app");
		args.push_back(PathText(*app));
	}
	if (windowless)
		args.push_back("--windowless");
	if (client == McpConfigurationClient::Codex)
	{
		auto output =
			"[mcp_servers.openxr-simulator]\ncommand = " + Json(PathText(connector)).dump() + "\nargs = " + args.dump();
		// JSON string escapes are valid TOML, which also requires escaping DEL.
		for (auto offset = output.find('\x7f'); offset != std::string::npos; offset = output.find('\x7f', offset + 6))
			output.replace(offset, 1, "\\u007f");
		return output;
	}
	return Json({{"mcpServers", {{"openxr-simulator", {{"command", PathText(connector)}, {"args", std::move(args)}}}}}})
		.dump(2);
}
}
