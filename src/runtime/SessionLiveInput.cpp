#include "Pch.h"
#include "runtime/Session.h"

namespace simxr
{
CommandResult Session::SubmitLiveInput(SessionGeneration generation, CommandSource source,
									   const LiveInputSample& sample)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed || !m_state.running)
		return CommandResult::Closed;
	if (m_liveInputFailed)
		return CommandResult::Cancelled;
	if (generation != m_state.generation)
		return CommandResult::Conflict;
	if (!m_liveInputWorker.joinable())
		m_liveInputWorker = std::jthread(
			[this](std::stop_token stop)
			{
				WatchLiveInput(stop);
			});
	CommandResult result = CommandResult::Conflict;
	m_scheduler.WithClockRevision(generation, sample.clockRevision,
								  [&]
								  {
									  result = m_publication.Commands().SubmitLiveInput(source, sample);
								  });
	return result;
}

bool Session::SubmitDisplayPrediction(SessionGeneration generation, CommandSource source, std::uint64_t attachment,
									  std::uint64_t sequence, std::uint64_t clockRevision, RuntimeTime target,
									  Duration period)
{
	std::lock_guard lock(m_mutex);
	if (m_state.closed || !m_state.running || m_liveInputFailed || generation != m_state.generation ||
		source.kind != CommandSourceKind::Device || !source.id)
		return false;
	const auto input = m_publication.Commands().LiveInputState();
	if (input.owner != source || input.phase != LiveInputPhase::Active || input.connection != attachment ||
		input.sequence != sequence || input.clockRevision != clockRevision)
		return false;
	const bool accepted = m_scheduler.PublishExternalPrediction(
		generation, clockRevision, source.id, sequence, target, period,
		[&]
		{
			return m_display.PublishExternalPrediction(generation, source.id, sequence, target, period);
		});
	if (accepted)
		m_externalDisplaySource = source;
	return accepted;
}

void Session::WatchLiveInput(std::stop_token stop)
{
	try
	{
		while (!stop.stop_requested())
		{
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
			std::lock_guard lock(m_mutex);
			if (stop.stop_requested() || !m_state.running)
				return;
			const auto input = m_publication.Commands().LiveInputState();
			const auto revision = m_scheduler.LiveInputClockRevision();
			bool restoreFocus = false;
			if (input.owner && (!revision || *revision != input.clockRevision))
				restoreFocus = m_publication.ReleaseSource(*input.owner, false);
			restoreFocus = m_publication.ExpireLiveInput(LiveInputClock::now()) || restoreFocus;
			const auto current = m_publication.Commands().LiveInputState();
			if (m_externalDisplaySource &&
				(current.owner != m_externalDisplaySource || current.phase != LiveInputPhase::Active || !revision ||
				 *revision != current.clockRevision))
			{
				m_scheduler.ReleaseExternalPrediction(m_externalDisplaySource->id);
				m_display.ReleaseExternalPrediction(m_externalDisplaySource->id);
				m_externalDisplaySource.reset();
			}
			if (restoreFocus &&
				(m_state.state == XR_SESSION_STATE_VISIBLE || m_state.state == XR_SESSION_STATE_SYNCHRONIZED))
				Transition(XR_SESSION_STATE_FOCUSED, m_state.time);
		}
	}
	catch (...)
	{
		// Never let a worker exception terminate the application or retain controls.
		try
		{
			std::lock_guard lock(m_mutex);
			m_liveInputFailed = true;
			auto& commands = m_publication.Commands();
			if (const auto owner = commands.LiveInputState().owner)
				m_publication.ReleaseSource(*owner, false);
			commands.Recorder().Fail();
			if (m_externalDisplaySource)
			{
				m_scheduler.ReleaseExternalPrediction(m_externalDisplaySource->id);
				m_display.ReleaseExternalPrediction(m_externalDisplaySource->id);
				m_externalDisplaySource.reset();
			}
		}
		catch (...)
		{
		}
		std::fputs("Simulator live-input watchdog failed\n", stderr);
	}
}
} // namespace simxr
