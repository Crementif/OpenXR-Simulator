#pragma once

#include "app/CompanionSettings.h"

#include <cstdint>
#include <memory>
#include <optional>

namespace simxr::app
{
// Worker-only native process owner. The application supervisor retains it until
// exit, or releases observation during final application shutdown without killing.
class ChildProcess
{
  public:
	static std::unique_ptr<ChildProcess> Launch(const LaunchProfile& profile);
	~ChildProcess();
	ChildProcess(const ChildProcess&) = delete;
	ChildProcess& operator=(const ChildProcess&) = delete;
	std::uint64_t Id() const noexcept;
	std::optional<int> PollExit();

  private:
	ChildProcess();
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::app
