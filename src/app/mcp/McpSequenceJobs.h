#pragma once

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <unordered_set>

#include <nlohmann/json.hpp>

namespace simxr::app
{
class McpApplication;
class McpCancellation;
class McpDebug;

class McpSequenceJobs
{
  public:
	McpSequenceJobs() = default;
	~McpSequenceJobs();
	McpSequenceJobs(const McpSequenceJobs&) = delete;
	McpSequenceJobs& operator=(const McpSequenceJobs&) = delete;
	static nlohmann::json Definitions();
	static bool Handles(const std::string& name);
	std::optional<nlohmann::json> Call(McpApplication& app, McpDebug& debug, const std::string& name,
		const nlohmann::json& arguments, const std::shared_ptr<McpCancellation>& cancellation);
	void Close() noexcept;

  private:
	void Run(McpApplication& app, McpDebug& debug, nlohmann::json options,
		std::shared_ptr<McpCancellation> cancellation) noexcept;
	std::mutex m_lifecycle, m_mutex;
	std::thread m_worker;
	std::shared_ptr<McpCancellation> m_cancellation;
	nlohmann::json m_status = nullptr, m_options = nullptr;
	std::unordered_set<std::string> m_retired;
	bool m_closed = false;
};
}
