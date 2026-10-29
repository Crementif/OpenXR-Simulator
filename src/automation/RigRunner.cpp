#include "automation/RigRunner.h"
#include "Pch.h"

#include "automation/RecordingPlayback.h"
#include "automation/RigReport.h"
#include "automation/ScriptEngine.h"

namespace simxr::automation
{
void ValidateRigOptions(const RigOptions& options)
{
	if (options.name.size() > 1024 || options.name.find('\0') != std::string::npos)
		throw std::invalid_argument("Rig name exceeds 1024 bytes or contains NUL");
	if ((options.source.empty() == options.recordingArchive.empty()) || options.source.size() > MaxRigSourceBytes ||
		options.recordingArchive.native().find(std::filesystem::path::value_type{}) !=
			std::filesystem::path::string_type::npos ||
		options.memoryLimit < 1024 * 1024 || options.memoryLimit > 512 * 1024 * 1024 || options.deadline.count() <= 0 ||
		options.deadline > std::chrono::hours{1})
		throw std::invalid_argument("Invalid bounded rig options");
}

RigRunner::RigRunner(RigExchange exchange, std::function<void()> releaseTransport)
	: m_runtime(std::move(exchange), m_progress), m_releaseTransport(std::move(releaseTransport))
{}

RigRunner::~RigRunner()
{
	Cancel();
	Join();
}

void RigRunner::Start(RigOptions options)
{
	ValidateRigOptions(options);
	if (!options.session.value || !options.generation)
		throw std::invalid_argument("Rig requires a session and generation");
	if (m_worker.joinable())
		throw std::logic_error("Join the previous rig before starting another");
	m_options = std::move(options);
	m_runtime.Configure(m_options.session, m_options.generation);
	m_progress.Start(m_options);
	try
	{
		m_worker = std::thread([this] { Run(); });
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

RigPhase RigRunner::Phase() const
{
	return m_progress.Phase();
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

void RigRunner::Run()
{
	RigPhase phase = RigPhase::Complete;
	std::string error;
	try
	{
		m_runtime.ObserveCompatibility();
		if (!m_options.recordingArchive.empty())
			ReplayRecordingArchive(m_options.recordingArchive, m_options.seed, m_runtime, m_progress);
		else
		{
			ScriptEngine engine{m_runtime, m_progress, m_options};
			engine.Execute();
		}
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
	const auto cleanup = [&](auto action) {
		try
		{
			action();
		}
		catch (const std::exception& failure)
		{
			if (phase == RigPhase::Complete)
			{
				phase = RigPhase::Failed;
				error = failure.what();
			}
		}
		catch (...)
		{
			if (phase == RigPhase::Complete)
			{
				phase = RigPhase::Failed;
				error = "Unknown automation cleanup failure";
			}
		}
	};
	cleanup([&] {
		const auto cleanupError = m_runtime.Cleanup();
		if (!cleanupError.empty())
			throw std::runtime_error(cleanupError);
	});
	// Terminal status is published only after the transport releases native ownership.
	cleanup([&] {
		if (m_releaseTransport)
			m_releaseTransport();
	});
	m_progress.Finish(phase, std::move(error));
}

} // namespace simxr::automation
