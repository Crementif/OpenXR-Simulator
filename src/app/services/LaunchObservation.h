#pragma once

#include "app/services/LaunchTypes.h"

#include <mutex>
#include <string_view>

namespace simxr::app
{
class ProcessSupervisor;
class LaunchObservation
{
  public:
	explicit LaunchObservation(std::string profile);
	LaunchSnapshot Read() const;

  private:
	friend class ProcessSupervisor;
	void Running(std::uint64_t process);
	void Exited(int code);
	void Failed(std::string_view detail) noexcept;
	mutable std::mutex m_mutex;
	LaunchSnapshot m_snapshot;
};
} // namespace simxr::app
