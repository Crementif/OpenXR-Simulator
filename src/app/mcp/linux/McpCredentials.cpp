#include "Pch.h"
#include "app/mcp/McpCredentials.h"

#include <cstdlib>
#include <array>
#include <cerrno>
#include <stdexcept>
#include <sys/random.h>

namespace simxr::app
{
std::string TakeMcpToken()
{
	const auto* value = std::getenv("SIMXR_MCP_TOKEN");
	const std::string token = value ? value : "";
	if (::unsetenv("SIMXR_MCP_TOKEN") != 0)
		throw std::runtime_error("Cannot remove the MCP credential from the process environment");
	return token;
}

std::string NewMcpIdentifier()
{
	std::array<unsigned char, 32> bytes{};
	std::size_t offset = 0;
	while (offset < bytes.size())
	{
		const auto count = ::getrandom(bytes.data() + offset, bytes.size() - offset, 0);
		if (count < 0 && errno == EINTR)
			continue;
		if (count <= 0)
			throw std::runtime_error("Cannot generate a secure MCP identifier");
		offset += static_cast<std::size_t>(count);
	}
	static constexpr char hex[] = "0123456789abcdef";
	std::string result;
	result.reserve(bytes.size() * 2);
	for (const auto byte : bytes)
	{
		result.push_back(hex[byte >> 4]);
		result.push_back(hex[byte & 15]);
	}
	return result;
}
}
