#include "Pch.h"
#include "app/services/ProcessLauncher.h"
#include "app/services/LaunchObservation.h"
#include "app/services/ProcessSupervisor.h"

namespace simxr::app
{
ProcessLauncher::ProcessLauncher(ProcessSupervisor& supervisor) : m_supervisor(supervisor)
{
}
ProcessLauncher::~ProcessLauncher()
{
	CancelRestart();
}
bool ProcessLauncher::Launch(LaunchProfile profile)
{
	ValidateLaunchProfile(profile);
	if (profile.useBundledRuntime)
		throw std::invalid_argument("Resolve the bundled runtime before launching");
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

bool ProcessLauncher::RequestClose()
{
	std::lock_guard lock(m_mutex);
	return m_observation && m_observation->RequestClose();
}
bool ProcessLauncher::Restart(std::uint64_t expectedProcess, LaunchProfile profile)
{
	ValidateLaunchProfile(profile);
	if (profile.useBundledRuntime)
		throw std::invalid_argument("Resolve the bundled runtime before restarting");
	if (profile.name.empty() || profile.executable.empty())
		throw std::invalid_argument("Restart needs a named launch profile and executable");
	std::lock_guard lock(m_mutex);
	return m_observation && m_observation->RequestRestart(expectedProcess, std::move(profile));
}
bool ProcessLauncher::CancelRestart()
{
	std::lock_guard lock(m_mutex);
	return m_observation && m_observation->CancelRestart();
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
