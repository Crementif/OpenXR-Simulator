#pragma once

#include "app/services/LaunchTypes.h"

#include <mutex>
#include <chrono>
#include <optional>
#include <string_view>

namespace simxr::app
{
class ProcessSupervisor;
class LaunchObservation
{
  public:
	explicit LaunchObservation(std::string profile);
	LaunchSnapshot Read() const;
	bool RequestClose();
	bool RequestRestart(std::uint64_t process, LaunchProfile profile);
	bool CancelRestart();

  private:
	friend class ProcessSupervisor;
	void Running(std::uint64_t process);
	std::optional<LaunchProfile> Exited(int code);
	void ExpireRestart();
	void Failed(std::string_view detail) noexcept;
	mutable std::mutex m_mutex;
	LaunchSnapshot m_snapshot;
	bool m_closeRequested = false, m_closeDelivered = false;
	std::optional<LaunchProfile> m_restart;
	std::chrono::steady_clock::time_point m_restartDeadline;
	bool TakeCloseRequest();
	void CloseFailed(std::string_view detail);
};
} // namespace simxr::app
