#include "Pch.h"
#include "app/services/ProcessLauncher.h"
#include "app/services/LaunchObservation.h"
#include "app/services/ProcessSupervisor.h"

#include <mutex>
#include <stdexcept>

namespace simxr::app
{
ProcessLauncher::ProcessLauncher(ProcessSupervisor& supervisor) : m_supervisor(supervisor)
{
}
ProcessLauncher::~ProcessLauncher() = default;
bool ProcessLauncher::Launch(LaunchProfile profile)
{
	ValidateLaunchProfile(profile);
	if (profile.name.empty())
		throw std::invalid_argument("Launch profile needs a name");
	if (profile.executable.empty())
		throw std::invalid_argument("Launch profile needs an executable");
	std::lock_guard lock(m_mutex);
	if (m_observation)
	{
		const auto state = m_observation->Read().state;
		if (state == LaunchState::Starting || state == LaunchState::Running)
			return false;
	}
	m_observation = m_supervisor.Submit(std::move(profile));
	return true;
}
LaunchSnapshot ProcessLauncher::Snapshot() const
{
	std::shared_ptr<LaunchObservation> observation;
	{
		std::lock_guard lock(m_mutex);
		observation = m_observation;
	}
	if (observation)
		return observation->Read();
	return {};
}
} // namespace simxr::app
