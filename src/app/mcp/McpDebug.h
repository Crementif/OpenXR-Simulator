#pragma once

#include <memory>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace simxr::app
{
class McpApplication;
class McpCancellation;

// One MCP session owns these runtime connections and their debugger leases.
class McpDebug
{
  public:
	McpDebug() = default;
	~McpDebug();
	McpDebug(const McpDebug&) = delete;
	McpDebug& operator=(const McpDebug&) = delete;
	static nlohmann::json Definitions();
	static bool Handles(const std::string& name);
	std::optional<nlohmann::json> Call(McpApplication& application, const std::string& name,
										 const nlohmann::json& arguments,
										 const std::shared_ptr<McpCancellation>& cancellation);
	std::vector<std::byte> ReadArtifact(McpApplication& application, const std::string& connection,
										 std::uint64_t ticket, std::uint64_t offset, std::uint64_t length,
										 const std::shared_ptr<McpCancellation>& cancellation = {});
	void Close() noexcept;

  private:
	struct Entry;
	std::mutex m_mutex;
	std::unordered_map<std::string, std::shared_ptr<Entry>> m_entries;
	bool m_closed = false;
};
} // namespace simxr::app
