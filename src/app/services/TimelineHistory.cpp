#include "app/services/TimelineHistory.h"
#include "Pch.h"

namespace simxr::app
{
static const char* PhaseName(automation::RigPhase phase)
{
	switch (phase)
	{
	case automation::RigPhase::Idle:
		return "Idle";
	case automation::RigPhase::Running:
		return "Running";
	case automation::RigPhase::Paused:
		return "Paused";
	case automation::RigPhase::Complete:
		return "Complete";
	case automation::RigPhase::Cancelled:
		return "Cancelled";
	case automation::RigPhase::Failed:
		return "Failed";
	}
	return "Unknown";
}
static TimelinePoint PointOf(const SessionView& state)
{
	return {{state.session, state.generation, state.latestFrame}, state.epoch, state.historySeconds};
}
RuntimeFrameRun TimelinePoint::Run() const
{
	return {epoch, identity.session, identity.run};
}
std::optional<std::pair<std::size_t, std::size_t>> TimelineSpan::Range(const HistoryStore& history) const
{
	if (first.Run() != last.Run())
		return {};
	const auto begin = history.Find(first.identity, first.epoch), end = history.Find(last.identity, last.epoch);
	if (!begin || !end || *end < *begin)
		return {};
	return std::pair{*begin, *end};
}
void TimelineHistory::Push(RuntimeFrameRun run, double seconds, std::string text, bool failed)
{
	m_events.push_back({run, std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count(),
						seconds, std::move(text), failed});
	if (m_events.size() > 128)
		m_events.erase(m_events.begin());
}
void TimelineHistory::Error(std::string text, double seconds)
{
	Push({}, seconds, std::move(text), true);
}
TimelineSpan TimelineHistory::Start(TimelinePoint point, bool replay)
{
	return {point, point, m_nextSpan++, replay};
}
void TimelineHistory::Close(std::optional<TimelineSpan>& span, std::vector<TimelineSpan>& destination)
{
	if (!span)
		return;
	destination.push_back(std::move(*span));
	span.reset();
	if (destination.size() > 128)
		destination.erase(destination.begin());
}
void TimelineHistory::Observe(const SessionView& state, bool recording)
{
	const auto point = PointOf(state);
	const auto run = point.Run();
	const bool changed = m_previous && (!state.connected || m_previous->point.Run() != run);
	if (changed)
	{
		if (m_previous->connected)
			Push(m_previous->point.Run(), m_previous->point.seconds, "Disconnected");
		Close(m_recording, m_clips);
		Close(m_script, m_clips);
		Close(m_pause, m_pauses);
		m_pauseEnd = 0;
	}
	if (state.connected && (!m_previous || !m_previous->connected || m_previous->point.Run() != run))
		Push(run, point.seconds, "Connected");
	else if (state.connected && m_previous && state.manual != m_previous->manual)
		Push(run, point.seconds, state.manual ? "Paused" : "Continued");
	for (auto* span : {&m_recording, &m_script})
		if (*span && (*span)->first.Run() == run && point.identity.frame)
			(*span)->last = point;
	if (m_pause && state.manual && m_pause->first.Run() == run && point.identity.frame)
		m_pause->last = point;
	const bool available = state.connected && state.sessionAvailable && point.identity.frame;
	if (available && recording && !m_recording)
	{
		m_recording = Start(point);
		Push(run, point.seconds, "Recording started");
	}
	if (!recording && m_recording)
	{
		Close(m_recording, m_clips);
		Push(run, point.seconds, "Recording stopped");
	}
	if (available && state.manual && !m_pause)
		m_pause = Start(point);
	if (!state.manual && m_pause)
	{
		m_pauseEnd = m_pause->id;
		Close(m_pause, m_pauses);
	}
	if (m_pauseEnd && !m_pauses.empty())
	{
		auto& pause = m_pauses.back();
		if (pause.id != m_pauseEnd || pause.last.Run() != run)
			m_pauseEnd = 0;
		else if (point.identity.frame > pause.last.identity.frame)
		{
			pause.last = point;
			m_pauseEnd = 0;
		}
	}
	m_previous = Observation{point, state.connected, state.manual};
}
void TimelineHistory::ObserveScript(const SessionView& state, std::uint64_t run, const automation::RigSnapshot& script,
									bool matchesTarget)
{
	if (!run)
		return;
	const auto point = PointOf(state);
	const bool newRun = run != m_scriptRun;
	if (newRun)
	{
		Close(m_script, m_clips);
		m_scriptRun = run;
		m_scriptTarget.reset();
		if (m_takeRun == run)
			m_scriptTarget = m_takeStart.Run();
		m_output = m_checkpoints = 0;
		m_phase = automation::RigPhase::Idle;
	}
	if (!m_scriptTarget && state.connected && matchesTarget)
		m_scriptTarget = point.Run();
	const bool matches = state.connected && matchesTarget && m_scriptTarget == point.Run();
	const auto append = [&](std::string text, bool failed = false)
	{
		Push(matches ? point.Run() : RuntimeFrameRun{}, point.seconds,
			 "Script " + std::to_string(run) + ": " + std::move(text), failed);
	};
	if (newRun)
		append("Started");
	const bool active = script.phase == automation::RigPhase::Running || script.phase == automation::RigPhase::Paused;
	if (matches && newRun && m_takeRun == run && m_takeStart.identity.frame)
	{
		m_script = Start(m_takeStart, true);
		m_script->scriptRun = run;
		m_script->take = m_takePath;
		if (point.identity.frame)
			m_script->last = point;
	}
	if (!matches || !active)
		Close(m_script, m_clips);
	else if (!m_script && point.identity.frame)
	{
		m_script = Start(point, true);
		m_script->scriptRun = run;
		if (m_takeRun == run)
			m_script->take = m_takePath;
	}
	if (script.phase != m_phase)
	{
		m_phase = script.phase;
		append(PhaseName(script.phase), script.phase == automation::RigPhase::Failed);
		if (script.phase == automation::RigPhase::Failed && !script.error.empty())
			append(script.error, true);
	}
	while (m_checkpoints < script.checkpoints.size())
	{
		const auto& checkpoint = script.checkpoints[m_checkpoints++];
		append(checkpoint.name + ": " + checkpoint.detail, !checkpoint.passed);
	}
	while (m_output < script.output.size())
		append(script.output[m_output++]);
}
void TimelineHistory::Resolve(const HistoryStore& history)
{
	const auto resolve = [&](TimelineSpan& span)
	{
		for (auto* point : {&span.first, &span.last})
			if (const auto index = history.Find(point->identity, point->epoch))
				point->seconds = history.At(*index).seconds;
	};
	for (auto* span : {&m_recording, &m_script, &m_pause})
		if (*span)
			resolve(**span);
	for (auto& span : m_clips)
		resolve(span);
	for (auto& span : m_pauses)
		resolve(span);
}
void TimelineHistory::SavedRecording(std::string path, RuntimeFrameRun run)
{
	for (auto span = m_clips.rbegin(); span != m_clips.rend(); ++span)
		if (!span->replay && span->first.Run() == run)
		{
			if (span->take.empty())
				span->take = std::move(path);
			break;
		}
}
void TimelineHistory::ReplayTake(std::uint64_t run, std::string path, const SessionView& launch)
{
	m_takeRun = run;
	m_takePath = std::move(path);
	m_takeStart = PointOf(launch);
}
const TimelineSpan* TimelineHistory::FindScript(std::uint64_t run) const
{
	if (!run)
		return nullptr;
	if (m_script && m_script->scriptRun == run)
		return &*m_script;
	for (auto span = m_clips.rbegin(); span != m_clips.rend(); ++span)
		if (span->scriptRun == run)
			return &*span;
	return nullptr;
}
} // namespace simxr::app
