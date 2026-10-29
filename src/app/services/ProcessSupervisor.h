#pragma once

#include "app/services/LaunchTypes.h"

#include <condition_variable>
#include <cstddef>
#include <memory>
#include <mutex>
#include <thread>
#include <vector>

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
	class Job;
	static constexpr std::size_t s_capacity = 64;
	void Run();
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::thread m_worker;
	std::vector<std::unique_ptr<Job>> m_jobs;
	bool m_stopping = false;
};
} // namespace simxr::app
