#pragma once

#include "app/services/ApplicationCatalog.h"

#include <condition_variable>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <unordered_map>

#include <nlohmann/json.hpp>

namespace simxr::app
{

// Cancellation can prevent queued mutations; once admitted, their result is authoritative.
class McpCancellation
{
  public:
	void Cancel() noexcept;
	bool Requested() noexcept;
	bool TryAdmit() noexcept;

  private:
	friend class McpApplication;
	enum class State { Queued, Running, Cancelled, Complete };
	State m_state = State::Queued;
	bool m_requested = false;
	std::mutex m_mutex;
	std::condition_variable m_changed;
};

// HTTP workers submit bounded work; the application thread owns its tabs and services.
class McpApplication
{
  public:
	explicit McpApplication(bool windowless = false);
	~McpApplication();
	McpApplication(const McpApplication&) = delete;
	McpApplication& operator=(const McpApplication&) = delete;
	std::optional<nlohmann::json> Call(std::string name, nlohmann::json arguments,
									 const std::shared_ptr<McpCancellation>& cancellation,
									 std::shared_ptr<AutomationOwner> owner = {});
	std::optional<ApplicationSnapshot> Lookup(const std::string& application,
		const std::shared_ptr<McpCancellation>& cancellation);
	nlohmann::json ApplicationDocument(const ApplicationSnapshot& application) const;
	void Poll(ApplicationCatalog& applications);
	void Close() noexcept;
	bool Windowless() const noexcept;

  private:
	struct Request;
	bool Wait(const std::shared_ptr<Request>& request);
	std::mutex m_mutex;
	std::deque<std::shared_ptr<Request>> m_pending;
	std::string m_instanceId;
	bool m_closed = false, m_windowless = false;
};
} // namespace simxr::app
