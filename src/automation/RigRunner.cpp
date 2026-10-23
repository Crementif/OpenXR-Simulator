#include "automation/RigRunner.h"
#include "Pch.h"

#include "automation/RigReport.h"
#include "automation/ScriptEngine.h"

#include <stdexcept>

namespace simxr::automation
{
void RigRunner::Run()
{
	RigPhase phase = RigPhase::Complete;
	std::string error;
	try
	{
		m_runtime.ObserveCompatibility();
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

RigRunner::RigRunner(RigExchange exchange) : m_runtime(std::move(exchange), m_progress) {}
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
	if (m_worker.joinable())
		throw std::logic_error("Join the previous rig before starting another");
	m_options = std::move(options);
	m_runtime.Configure(m_options.session, m_options.generation);
	m_progress.Start(m_options);
	try
	{
		m_worker = std::thread(
			[this]
			{
				Run();
			});
	}
	catch (...)
	{
		m_progress.Finish(RigPhase::Failed, "Cannot start rig worker");
		throw;
	}
}
void RigRunner::Pause()
{
	m_progress.Pause();
}
void RigRunner::Resume()
{
	m_progress.Resume();
}
void RigRunner::Step()
{
	m_progress.Step();
}
void RigRunner::Cancel()
{
	m_progress.Cancel();
}
void RigRunner::Join()
{
	if (m_worker.joinable())
		m_worker.join();
}
RigSnapshot RigRunner::Snapshot() const
{
	return m_progress.Snapshot();
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
