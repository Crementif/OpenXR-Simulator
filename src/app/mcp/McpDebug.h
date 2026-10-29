#pragma once

#include "control/ControlPacket.h"

#include <memory>
#include <cstddef>
#include <cstdint>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <unordered_map>
#include <vector>

#include <nlohmann/json.hpp>

namespace simxr::app
{
class McpApplication;
class McpCancellation;

namespace mcp
{
struct ImageRenderRequest;
struct ImageRenderResult;
}

struct McpConnection
{
	std::string id;
	std::uint64_t process = 0, epoch = 0, session = 0, generation = 0, capabilities = 0;
};

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
	static nlohmann::json Document(const control::Message& message, bool metadataOnly = false);

	std::optional<McpConnection> Connect(McpApplication& application, const std::string& target,
										 const std::shared_ptr<McpCancellation>& cancellation, bool gameOnly = false);
	void Disconnect(const std::string& connection) noexcept;
	void Close() noexcept;

	std::optional<nlohmann::json> Call(McpApplication& application, const std::string& name,
									   const nlohmann::json& arguments,
									   const std::shared_ptr<McpCancellation>& cancellation);
	control::Message Request(McpApplication& application, const std::string& connection, control::OpCode opcode,
							 std::vector<control::Field> fields,
							 const std::shared_ptr<McpCancellation>& cancellation = {},
							 std::optional<std::string_view> program = {});
	mcp::ImageRenderResult Render(McpApplication& application, const std::string& connection,
								  const mcp::ImageRenderRequest& render,
								  const std::shared_ptr<McpCancellation>& cancellation);
	std::vector<std::byte> ReadArtifact(McpApplication& application, const std::string& connection,
										std::uint64_t ticket, std::uint64_t offset, std::uint64_t length,
										const std::shared_ptr<McpCancellation>& cancellation = {});

private:
	struct Entry;

	std::shared_ptr<Entry> FindConnection(const std::string& connection);
	std::unique_lock<std::timed_mutex> LockConnection(McpApplication& application, const std::string& connection,
													  const std::shared_ptr<Entry>& entry,
													  const std::shared_ptr<McpCancellation>& cancellation);

	std::mutex m_mutex;
	std::unordered_map<std::string, std::shared_ptr<Entry>> m_entries;
	bool m_closed = false;
};
} // namespace simxr::app
