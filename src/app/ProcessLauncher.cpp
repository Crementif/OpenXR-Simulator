#include "app/ProcessLauncher.h"
#include "app/LaunchObservation.h"
#include "app/ProcessSupervisor.h"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace simxr::app
{
struct ProcessLauncher::Impl
{
	ProcessSupervisor& supervisor;
	mutable std::mutex mutex;
	std::shared_ptr<LaunchObservation> observation;
	explicit Impl(ProcessSupervisor& supervisor) : supervisor(supervisor) {}
};
ProcessLauncher::ProcessLauncher(ProcessSupervisor& supervisor) : m_impl(std::make_unique<Impl>(supervisor)) {}
ProcessLauncher::~ProcessLauncher() = default;
bool ProcessLauncher::Launch(LaunchProfile profile)
{
	CompanionSettings validation;
	validation.profiles.push_back(profile);
	ValidateSettings(validation);
	if (profile.executable.empty())
		throw std::invalid_argument("Launch profile needs an executable");
	std::lock_guard lock(m_impl->mutex);
	if (m_impl->observation)
	{
		const auto state = m_impl->observation->Read().state;
		if (state == LaunchState::Starting || state == LaunchState::Running)
			return false;
	}
	m_impl->observation = m_impl->supervisor.Submit(std::move(profile));
	return true;
}
LaunchSnapshot ProcessLauncher::Snapshot() const
{
	std::shared_ptr<LaunchObservation> observation;
	{
		std::lock_guard lock(m_impl->mutex);
		observation = m_impl->observation;
	}
	if (observation)
		return observation->Read();
	return {};
}
} // namespace simxr::app
