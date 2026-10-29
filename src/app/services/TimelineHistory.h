#pragma once

#include "app/services/HistoryStore.h"
#include "app/services/SessionView.h"
#include "automation/RigRunner.h"

#include <chrono>
#include <cstdint>
#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace simxr::app
{
struct TimelinePoint
{
	FrameIdentity identity;
	std::uint64_t epoch = 0;
	double seconds = 0;
	RuntimeFrameRun Run() const;
};

struct TimelineSpan
{
	TimelinePoint first, last;
	std::uint64_t id = 0;
	bool replay = false;
	std::string take;
	std::uint64_t scriptRun = 0;
	std::optional<std::pair<std::size_t, std::size_t>> Range(const HistoryStore& history) const;
};

struct TimelineEvent
{
	RuntimeFrameRun run;
	double seconds = 0, axisSeconds = 0;
	std::string text;
	bool failed = false;
};

// Bounded observations retain their original frame endpoints even after eviction or reconnect.
class TimelineHistory
{
public:
	void Observe(const SessionView& state, bool recording);
	void ObserveScript(const SessionView& state, std::uint64_t run, const automation::RigSnapshot& script,
					   bool matchesTarget);
	void Resolve(const HistoryStore& history);
	void SavedRecording(std::string path, RuntimeFrameRun run);
	void ReplayTake(std::uint64_t run, std::string path, const SessionView& launch);
	const TimelineSpan* FindScript(std::uint64_t run) const;
	void Error(std::string text, double seconds);

	const std::vector<TimelineSpan>& Clips() const
	{
		return m_clips;
	}

	const std::vector<TimelineSpan>& Pauses() const
	{
		return m_pauses;
	}

	const std::vector<TimelineEvent>& Events() const
	{
		return m_events;
	}

	const std::optional<TimelineSpan>& Recording() const
	{
		return m_recording;
	}

	const std::optional<TimelineSpan>& Script() const
	{
		return m_script;
	}

	const std::optional<TimelineSpan>& Pause() const
	{
		return m_pause;
	}

	std::uint64_t ScriptRun() const
	{
		return m_scriptRun;
	}

private:
	struct Observation
	{
		TimelinePoint point;
		bool connected = false, manual = false;
	};

	void Push(RuntimeFrameRun run, double seconds, std::string text, bool failed = false);
	TimelineSpan Start(TimelinePoint point, bool replay = false);
	static void Close(std::optional<TimelineSpan>& span, std::vector<TimelineSpan>& destination);
	std::optional<Observation> m_previous;
	std::optional<TimelineSpan> m_recording, m_script, m_pause;
	std::vector<TimelineSpan> m_clips, m_pauses;
	std::vector<TimelineEvent> m_events;
	std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now();
	std::optional<RuntimeFrameRun> m_scriptTarget;
	std::uint64_t m_nextSpan = 1, m_scriptRun = 0, m_pauseEnd = 0, m_takeRun = 0;
	std::string m_takePath;
	TimelinePoint m_takeStart;
	automation::RigPhase m_phase = automation::RigPhase::Idle;
	std::size_t m_output = 0, m_checkpoints = 0;
};
} // namespace simxr::app
