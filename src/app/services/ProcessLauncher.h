#pragma once

#include "app/services/LaunchTypes.h"

#include <memory>
#include <mutex>

namespace simxr::app
{
class ProcessSupervisor;
class LaunchObservation;

class ProcessLauncher
{
public:
	// The application supervisor must outlive this UI/controller observation.
	explicit ProcessLauncher(ProcessSupervisor& supervisor);
	~ProcessLauncher();
	ProcessLauncher(const ProcessLauncher&) = delete;
	ProcessLauncher& operator=(const ProcessLauncher&) = delete;
	bool Launch(LaunchProfile profile);
	// Only the currently observed child; acceptance is not proof of process exit.
	bool RequestClose();
	// Relaunches after observed exit, or cancels after ten seconds without exit.
	bool Restart(std::uint64_t expectedProcess, LaunchProfile profile);
	// Cancels only a queued relaunch; an already delivered close cannot be undone.
	bool CancelRestart();
	LaunchSnapshot Snapshot() const;

private:
	ProcessSupervisor& m_supervisor;
	mutable std::mutex m_mutex;
	std::shared_ptr<LaunchObservation> m_observation;
};
} // namespace simxr::app
