#include "automation/RigRunner.h"
#include "automation/RigProgress.h"
#include "automation/RigReport.h"
#include "automation/RigRuntime.h"
#include "automation/ScriptEngine.h"

#include <stdexcept>
#include <thread>

namespace simxr::automation
{
struct RigRunner::Impl
{
	RigOptions m_options;
	RigProgress m_progress;
	RigRuntime m_runtime;
	std::thread m_worker;

	explicit Impl(RigExchange exchange) : m_runtime(std::move(exchange), m_progress) {}
	void Run()
	{
		RigPhase phase = RigPhase::Complete;
		std::string error;
		try
		{
			ScriptEngine engine{m_runtime, m_progress, m_options};
			engine.Execute();
		}
		catch (const std::exception& failure)
		{
			phase = m_progress.Cancelled() ? RigPhase::Cancelled : RigPhase::Failed;
			error = failure.what();
		}
		catch (...)
		{
			phase = RigPhase::Failed;
			error = "Unknown rig failure";
		}
		const auto cleanupError = m_runtime.Cleanup();
		if (phase == RigPhase::Complete && !cleanupError.empty())
		{
			phase = RigPhase::Failed;
			error = cleanupError;
		}
		m_progress.Finish(phase, std::move(error));
	}
};

RigRunner::RigRunner(RigExchange exchange) : m_impl(std::make_unique<Impl>(std::move(exchange))) {}
RigRunner::~RigRunner()
{
	Cancel();
	Join();
}
void RigRunner::Start(RigOptions options)
{
	if (!options.session.value || !options.generation || options.source.empty() ||
		options.source.size() > control::MaxPayload || options.name.size() > 1024 ||
		options.memoryLimit < 1024 * 1024 || options.memoryLimit > 512 * 1024 * 1024 || options.deadline.count() <= 0 ||
		options.deadline > std::chrono::hours{1})
		throw std::invalid_argument("Invalid bounded rig options");
	if (m_impl->m_worker.joinable())
		throw std::logic_error("Join the previous rig before starting another");
	m_impl->m_options = std::move(options);
	m_impl->m_runtime.Configure(m_impl->m_options.session, m_impl->m_options.generation);
	m_impl->m_progress.Start(m_impl->m_options);
	try
	{
		m_impl->m_worker = std::thread(
			[impl = m_impl.get()]
			{
				impl->Run();
			});
	}
	catch (...)
	{
		m_impl->m_progress.Finish(RigPhase::Failed, "Cannot start rig worker");
		throw;
	}
}
void RigRunner::Pause()
{
	m_impl->m_progress.Pause();
}
void RigRunner::Resume()
{
	m_impl->m_progress.Resume();
}
void RigRunner::Step()
{
	m_impl->m_progress.Step();
}
void RigRunner::Cancel()
{
	m_impl->m_progress.Cancel();
}
void RigRunner::Join()
{
	if (m_impl->m_worker.joinable())
		m_impl->m_worker.join();
}
RigSnapshot RigRunner::Snapshot() const
{
	return m_impl->m_progress.Snapshot();
}
std::string RigRunner::JsonReport() const
{
	return SerializeRigJson(Snapshot());
}
std::string RigRunner::JunitReport() const
{
	return SerializeRigJunit(Snapshot());
}
} // namespace simxr::automation
