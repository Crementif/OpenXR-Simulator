#pragma once

#include "app/RuntimePanels.h"
#include "app/TimelineCanvas.h"
#include "automation/RigRunner.h"
#include <array>
#include <chrono>
#include <map>
#include <string>
#include <vector>

namespace simxr::app
{
// Transport, retained-frame navigation and bounded, explicitly observed events.
class TimelinePanel
{
  public:
	bool Draw(RuntimePanels& runtime, ShellState& shell);
	void ObserveRig(const RuntimeView& runtime, std::uint64_t run, const automation::RigSnapshot& state,
					bool matchesTarget);

  private:
	struct Event
	{
		// Empty for events of a script that drives another app or session.
		RuntimeFrameRun run;
		double seconds = 0, axisSeconds = 0;
		std::string text;
		bool failed = false;
	};
	// Markers keep their frame's run identity; the canvas time orders them.
	struct Marker
	{
		HistoryFrame frame;
		std::string name;
	};
	struct Span
	{
		double first = 0, last = 0;
		bool replay = false;
	};
	void Observe(const RuntimeView& state);
	void Push(Event event);
	std::map<double, Marker> m_markers;
	std::optional<HistoryFrame> m_pendingMarker;
	std::array<char, 96> m_markerName{};
	std::vector<Span> m_clips, m_pauses;
	std::optional<double> m_recordingBegin, m_scriptBegin, m_pauseBegin, m_pauseEnd;
	// The runtime's last submitted frame when the pause was first seen.
	std::optional<HistoryFrame> m_pauseFrame;
	std::optional<RuntimeView> m_previous;
	double m_cursorSeconds = 0;
	std::vector<Event> m_events;
	std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now();
	std::uint64_t m_rigRun = 0;
	automation::RigPhase m_phase = automation::RigPhase::Idle;
	std::size_t m_output = 0, m_checkpoints = 0;
	TimelineCanvas m_canvas;
};
} // namespace simxr::app
