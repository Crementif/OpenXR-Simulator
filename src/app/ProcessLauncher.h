#pragma once

#include "app/CompanionSettings.h"
#include "app/LaunchTypes.h"

#include <memory>

namespace simxr::app
{
class ProcessSupervisor;
class ProcessLauncher
{
  public:
	// The application supervisor must outlive this UI/controller observation.
	explicit ProcessLauncher(ProcessSupervisor& supervisor);
	~ProcessLauncher();
	ProcessLauncher(const ProcessLauncher&) = delete;
	ProcessLauncher& operator=(const ProcessLauncher&) = delete;
	bool Launch(LaunchProfile profile);
	LaunchSnapshot Snapshot() const;

  private:
	struct Impl;
	std::unique_ptr<Impl> m_impl;
};
} // namespace simxr::app
