#pragma once

#include <atomic>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>

namespace simxr::app
{
enum class UpdateStatus
{
	Idle,
	Checking,
	Current,
	Available,
	Unavailable
};
struct UpdateResult
{
	UpdateStatus status = UpdateStatus::Idle;
	std::string version, releaseUrl, error;
};
struct UpdateUiState
{
	UpdateResult result;
	bool checkRequested = false;
};
bool ValidReleaseRepository(std::string_view repository);
UpdateResult ParseRelease(std::string_view repository, std::string_view current, std::string_view response);
std::string FetchLatestRelease(std::string_view repository);

class UpdateChecker
{
  public:
	using Fetch = std::function<std::string(std::string_view)>;
	UpdateChecker(std::string repository, std::string version, Fetch fetch = FetchLatestRelease);
	~UpdateChecker();
	UpdateChecker(const UpdateChecker&) = delete;
	UpdateChecker& operator=(const UpdateChecker&) = delete;
	void Start();
	// Returns true exactly once when an asynchronous request completes.
	bool Poll();
	const UpdateResult& State() const noexcept;

  private:
	std::string m_repository, m_version;
	Fetch m_fetch;
	UpdateResult m_state;
	std::mutex m_mutex;
	std::optional<UpdateResult> m_completed;
	std::atomic<bool> m_done = false;
	std::thread m_worker;
};
} // namespace simxr::app
