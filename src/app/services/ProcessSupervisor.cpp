#include "Pch.h"
#include "app/services/ProcessSupervisor.h"
#include "app/platform/ChildProcess.h"
#include "app/services/LaunchObservation.h"

namespace simxr::app
{
LaunchObservation::LaunchObservation(std::string profile)
{
	m_snapshot.state = LaunchState::Starting;
	m_snapshot.profile = std::move(profile);
	m_snapshot.detail.reserve(1024);
	m_snapshot.detail = "Launching process";
}

LaunchSnapshot LaunchObservation::Read() const
{
	std::lock_guard lock(m_mutex);
	return m_snapshot;
}

bool LaunchObservation::RequestClose()
{
	std::lock_guard lock(m_mutex);
	if (m_snapshot.state != LaunchState::Running || m_closeRequested || m_closeDelivered)
		return false;
	m_closeRequested = true;
	m_snapshot.detail = "Close requested; waiting for the app to exit";
	return true;
}

bool LaunchObservation::RequestRestart(std::uint64_t process, LaunchProfile profile)
{
	std::lock_guard lock(m_mutex);
	if (!process || m_snapshot.process != process || m_snapshot.state != LaunchState::Running || m_restart ||
		m_closeRequested || m_closeDelivered)
		return false;
	m_restart = std::move(profile);
	m_restartDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	m_closeRequested = true;
	m_snapshot.restartPending = true;
	m_snapshot.detail = "Restart requested; waiting for the app to exit";
	return true;
}

bool LaunchObservation::CancelRestart()
{
	std::lock_guard lock(m_mutex);
	if (!m_restart)
		return false;
	m_restart.reset();
	m_closeRequested = false;
	m_snapshot.restartPending = false;
	m_snapshot.detail = "Restart cancelled; a delivered close request cannot be undone";
	return true;
}

void LaunchObservation::ExpireRestart()
{
	std::lock_guard lock(m_mutex);
	if (m_restart && std::chrono::steady_clock::now() >= m_restartDeadline)
	{
		m_restart.reset();
		m_closeRequested = false;
		m_snapshot.restartPending = false;
		m_snapshot.detail = "Restart cancelled: the app did not exit within ten seconds";
	}
}

bool LaunchObservation::TakeCloseRequest()
{
	std::lock_guard lock(m_mutex);
	if (!std::exchange(m_closeRequested, false))
		return false;
	m_closeDelivered = true;
	return true;
}

void LaunchObservation::CloseFailed(std::string_view detail)
{
	std::lock_guard lock(m_mutex);
	m_closeDelivered = false;
	m_restart.reset();
	m_snapshot.restartPending = false;
	m_snapshot.detail.assign(detail.substr(0, 1024));
}

void LaunchObservation::Running(std::uint64_t process)
{
	std::lock_guard lock(m_mutex);
	m_snapshot.process = process;
	m_snapshot.state = LaunchState::Running;
	m_snapshot.detail = "The app is running. Open Connection to connect.";
}

std::optional<LaunchProfile> LaunchObservation::Exited(int code)
{
	std::array<char, 32> digits;
	const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), code);
	std::lock_guard lock(m_mutex);
	if (m_restart && std::chrono::steady_clock::now() < m_restartDeadline)
	{
		auto profile = std::move(m_restart);
		m_restart.reset();
		m_snapshot.state = LaunchState::Starting;
		m_snapshot.process = 0;
		m_snapshot.profile = profile->name;
		m_snapshot.restartPending = false;
		m_closeRequested = m_closeDelivered = false;
		m_snapshot.detail = "Previous app exited; launching replacement";
		return profile;
	}
	m_restart.reset();
	m_snapshot.restartPending = false;
	m_snapshot.state = LaunchState::Exited;
	m_snapshot.exitCode = code;
	m_snapshot.detail = "Process exited with code ";
	m_snapshot.detail.append(digits.data(), converted.ptr);
	return {};
}

void LaunchObservation::Failed(std::string_view detail) noexcept
{
	std::lock_guard lock(m_mutex);
	m_snapshot.state = LaunchState::Failed;
	m_restart.reset();
	m_snapshot.restartPending = false;
	m_snapshot.detail.assign(detail.substr(0, 1024));
}

class ProcessSupervisor::Job
{
public:
	Job(LaunchProfile profile, std::shared_ptr<LaunchObservation> observation)
		: m_profile(std::move(profile)), m_observation(std::move(observation))
	{}

	void Update() noexcept
	{
		try
		{
			if (!m_process)
			{
				m_process = ChildProcess::Launch(m_profile);
				m_observation->Running(m_process->Id());
			}
			m_observation->ExpireRestart();
			if (m_observation->TakeCloseRequest())
			{
				try
				{
					m_process->RequestClose();
				}
				catch (const std::exception& error)
				{
					m_observation->CloseFailed(error.what());
				}
			}
			if (const auto exit = m_process->PollExit())
			{
				if (auto replacement = m_observation->Exited(*exit))
				{
					m_process.reset();
					m_profile = std::move(*replacement);
				}
				else
					m_finished = true;
			}
		}
		catch (const std::exception& error)
		{
			m_observation->Failed(error.what());
			m_finished = true;
		}
		catch (...)
		{
			m_observation->Failed("Unknown child process failure");
			m_finished = true;
		}
	}

	bool Finished() const noexcept
	{
		return m_finished;
	}

private:
	LaunchProfile m_profile;
	std::shared_ptr<LaunchObservation> m_observation;
	std::unique_ptr<ChildProcess> m_process;
	bool m_finished = false;
};

ProcessSupervisor::ProcessSupervisor()
{
	m_jobs.reserve(s_capacity);
}

ProcessSupervisor::~ProcessSupervisor()
{
	{
		std::lock_guard lock(m_mutex);
		m_stopping = true;
	}
	m_changed.notify_all();
	if (m_worker.joinable())
		m_worker.join();
}

void ProcessSupervisor::Run()
{
	for (;;)
	{
		std::array<Job*, s_capacity> observed{};
		std::array<std::unique_ptr<Job>, s_capacity> retired;
		std::size_t count = 0;
		{
			std::unique_lock lock(m_mutex);
			m_changed.wait(lock, [&] { return m_stopping || !m_jobs.empty(); });
			if (m_stopping)
				return;
			count = m_jobs.size();
			for (std::size_t index = 0; index < count; ++index)
				observed[index] = m_jobs[index].get();
		}
		for (std::size_t index = 0; index < count; ++index)
			observed[index]->Update();
		{
			std::unique_lock lock(m_mutex);
			std::size_t retiredCount = 0;
			std::erase_if(m_jobs, [&](auto& job) {
				if (!job->Finished())
					return false;
				retired[retiredCount++] = std::move(job);
				return true;
			});
			m_changed.wait_for(lock, std::chrono::milliseconds(50));
		}
	}
}

std::size_t ProcessSupervisor::Outstanding() const
{
	std::lock_guard lock(m_mutex);
	return m_jobs.size();
}

std::shared_ptr<LaunchObservation> ProcessSupervisor::Submit(LaunchProfile profile)
{
	auto observation = std::make_shared<LaunchObservation>(profile.name);
	auto job = std::make_unique<Job>(std::move(profile), observation);
	{
		std::lock_guard lock(m_mutex);
		if (m_stopping || m_jobs.size() == s_capacity)
			throw std::runtime_error("Child process observation capacity exhausted");
		if (!m_worker.joinable())
			m_worker = std::thread([this] { Run(); });
		m_jobs.push_back(std::move(job));
	}
	m_changed.notify_all();
	return observation;
}
} // namespace simxr::app
