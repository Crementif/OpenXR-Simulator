#pragma once

#include "app/CompanionSettings.h"

#include <cstddef>
#include <memory>

namespace simxr::app
{
class LaunchObservation;
class ProcessLauncher;
// Application-lifetime owner: panel/launcher destruction never stops observation.
// Final shutdown stops and joins its worker without terminating independent games.
class ProcessSupervisor
{
  public:
	ProcessSupervisor();
	~ProcessSupervisor();
	ProcessSupervisor(const ProcessSupervisor&) = delete;
	ProcessSupervisor& operator=(const ProcessSupervisor&) = delete;
	std::size_t Outstanding() const;

  private:
	friend class ProcessLauncher;
	std::shared_ptr<LaunchObservation> Submit(LaunchProfile profile);
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr::app
