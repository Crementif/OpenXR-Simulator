#include "Pch.h"
#include "app/mcp/McpCredentials.h"
#include "app/platform/windows/LaunchSupport.h"

#include <array>
#include <bcrypt.h>
#include <stdexcept>

namespace simxr::app
{
std::string TakeMcpToken()
{
	auto token = windows::Environment("SIMXR_MCP_TOKEN");
	if (!SetEnvironmentVariableW(L"SIMXR_MCP_TOKEN", nullptr) && GetLastError() != ERROR_ENVVAR_NOT_FOUND)
		throw windows::Error("Cannot remove the MCP credential from the process environment");
	return token;
}

std::string NewMcpIdentifier()
{
	std::array<unsigned char, 32> bytes{};
	if (BCryptGenRandom(nullptr, bytes.data(), static_cast<ULONG>(bytes.size()), BCRYPT_USE_SYSTEM_PREFERRED_RNG) < 0)
		throw std::runtime_error("Cannot generate a secure MCP identifier");
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
