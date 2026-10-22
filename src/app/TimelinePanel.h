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
	// Folded draws one row: transport, LIVE and the frame rate. fold is set when
	// the user asks to fold or unfold the timeline.
	bool Draw(RuntimePanels& runtime, ShellState& shell, bool folded, bool& fold);
	static float FoldedHeight(float scale)
	{
		return 36 * scale;
	}
	void ObserveRig(const RuntimeView& runtime, std::uint64_t run, const automation::RigSnapshot& state,
					bool matchesTarget);
	// A take is about to replay; when it was cut from history, its source is drawn over the replay.
	void ReplayTake(const std::string& path);

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
		// The saved take of a Recording clip, once known.
		std::string take;
	};
	// Source frames of a take cut from history, and where its replay began.
	struct Ghost
	{
		TakeSource source;
		// Requested at this canvas time; an earlier script is not the replay.
		double requested = 0;
		std::optional<double> begin;
		std::optional<HistoryFrame> anchor;
	};
	void Observe(const RuntimeView& state);
	void Play(RuntimePanels& runtime, const std::string& path);
	void DrawGhost(const HistoryStore& history);
	void Push(Event event);
	// The frame-rate meter in the LIVE column: numbers, and a sparkline when there is room.
	void DrawMeter(const HistoryStore& history, const RuntimeView& state, ShellRect area, bool oneRow);
	void DrawLegend();
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
	std::vector<TakeSource> m_takes;
	std::optional<Ghost> m_ghost;
	TimelineCanvas m_canvas;
	bool m_tint = false;
};
} // namespace simxr::app
