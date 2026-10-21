#include "app/ProcessSupervisor.h"
#include "app/ChildProcess.h"
#include "app/LaunchObservation.h"

#include <algorithm>
#include <array>
#include <charconv>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <stdexcept>
#include <thread>
#include <utility>

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
void LaunchObservation::Running(std::uint64_t process)
{
	std::lock_guard lock(m_mutex);
	m_snapshot.process = process;
	m_snapshot.state = LaunchState::Running;
	m_snapshot.detail = "Process running; attach using Runtime connection";
}
void LaunchObservation::Exited(int code)
{
	std::array<char, 32> digits;
	const auto converted = std::to_chars(digits.data(), digits.data() + digits.size(), code);
	std::lock_guard lock(m_mutex);
	m_snapshot.state = LaunchState::Exited;
	m_snapshot.exitCode = code;
	m_snapshot.detail = "Process exited with code ";
	m_snapshot.detail.append(digits.data(), converted.ptr);
}
void LaunchObservation::Failed(std::string_view detail) noexcept
{
	std::lock_guard lock(m_mutex);
	m_snapshot.state = LaunchState::Failed;
	m_snapshot.detail.assign(detail.substr(0, 1024));
}
struct ProcessSupervisor::State
{
	class Job
	{
	  public:
		Job(LaunchProfile profile, std::shared_ptr<LaunchObservation> observation)
			: m_profile(std::move(profile)), m_observation(std::move(observation))
		{
		}
		void Update() noexcept
		{
			try
			{
				if (!m_process)
				{
					m_process = ChildProcess::Launch(m_profile);
					m_observation->Running(m_process->Id());
				}
				if (const auto exit = m_process->PollExit())
				{
					m_observation->Exited(*exit);
					m_finished = true;
				}
			}
			catch (const std::exception& error)
			{
				m_observation->Failed(error.what());
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
	static constexpr std::size_t Capacity = 64;
	mutable std::mutex mutex;
	std::condition_variable changed;
	std::thread worker;
	std::vector<std::unique_ptr<Job>> jobs;
	bool stopping = false;
	State()
	{
		jobs.reserve(Capacity);
	}
	~State()
	{
		{
			std::lock_guard lock(mutex);
			stopping = true;
		}
		changed.notify_all();
		if (worker.joinable())
			worker.join();
	}
	void Run()
	{
		for (;;)
		{
			std::array<Job*, Capacity> observed{};
			std::array<std::unique_ptr<Job>, Capacity> retired;
			std::size_t count = 0;
			{
				std::unique_lock lock(mutex);
				changed.wait(lock,
							 [&]
							 {
								 return stopping || !jobs.empty();
							 });
				if (stopping)
					return;
				count = jobs.size();
				for (std::size_t index = 0; index < count; ++index)
					observed[index] = jobs[index].get();
			}
			for (std::size_t index = 0; index < count; ++index)
				observed[index]->Update();
			{
				std::unique_lock lock(mutex);
				std::size_t retiredCount = 0;
				std::erase_if(jobs,
							  [&](auto& job)
							  {
								  if (!job->Finished())
									  return false;
								  retired[retiredCount++] = std::move(job);
								  return true;
							  });
				changed.wait_for(lock, std::chrono::milliseconds(50));
			}
		}
	}
};
ProcessSupervisor::ProcessSupervisor() : m_state(std::make_unique<State>()) {}
ProcessSupervisor::~ProcessSupervisor() = default;
std::size_t ProcessSupervisor::Outstanding() const
{
	std::lock_guard lock(m_state->mutex);
	return m_state->jobs.size();
}
std::shared_ptr<LaunchObservation> ProcessSupervisor::Submit(LaunchProfile profile)
{
	auto observation = std::make_shared<LaunchObservation>(profile.name);
	auto job = std::make_unique<State::Job>(std::move(profile), observation);
	{
		std::lock_guard lock(m_state->mutex);
		if (m_state->stopping || m_state->jobs.size() == State::Capacity)
			throw std::runtime_error("Child process observation capacity exhausted");
		if (!m_state->worker.joinable())
			m_state->worker = std::thread(
				[state = m_state.get()]
				{
					state->Run();
				});
		m_state->jobs.push_back(std::move(job));
	}
	m_state->changed.notify_all();
	return observation;
}
} // namespace simxr::app
