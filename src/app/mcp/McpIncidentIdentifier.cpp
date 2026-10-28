#include "app/mcp/McpPch.h"
#include "app/mcp/McpIncident.h"

namespace simxr::app::mcp
{
std::uint64_t IncidentIdentifier(const nlohmann::json& value, const char* field)
{
	if (!value.is_string())
		throw std::runtime_error(std::string("Invalid ") + field);
	const auto& text = value.get_ref<const std::string&>();
	if (text.empty() || text.size() > 20 || text.front() == '0')
		throw std::runtime_error(std::string("Invalid ") + field);
	std::uint64_t result = 0;
	const auto parsed = std::from_chars(text.data(), text.data() + text.size(), result);
	if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || !result)
		throw std::runtime_error(std::string("Invalid ") + field);
	return result;
}
}
