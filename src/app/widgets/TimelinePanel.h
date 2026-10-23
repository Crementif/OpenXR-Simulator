#pragma once

#include "app/services/ShellState.h"
#include "app/services/TimelineHistory.h"
#include "app/widgets/TimelineCanvas.h"
#include "automation/RigRunner.h"
#include <array>
#include <map>
#include <string>
#include <vector>

namespace simxr::app
{
class RuntimePanels;
struct RuntimeView;
// Transport, retained-frame navigation and bounded, explicitly observed events.
class TimelinePanel
{
  public:
	// Folded draws one row: transport, LIVE and the frame rate. fold is set when
	// the user asks to fold or unfold the timeline.
	bool Draw(RuntimePanels& runtime, ShellState& shell, bool folded);
	static float FoldedHeight(float scale)
	{
		return 36 * scale;
	}
	// Screen rectangle of the Library drawer below the transport; empty while it is closed.
	ShellRect DrawerRect() const
	{
		return m_drawer;
	}
	void ObserveRig(const RuntimeView& runtime, std::uint64_t run, const automation::RigSnapshot& state,
					bool matchesTarget);
	void Observe(const RuntimeView& runtime);
	void SavedRecording(std::string path, const RuntimeView& runtime);
	// A take is about to replay; when it was cut from history, its source is drawn over the replay.
	void ReplayTake(const std::string& path, const RuntimeView& state, std::uint64_t scriptRun);

  private:
	// Markers keep their frame's run identity; the canvas time orders them.
	struct Marker
	{
		HistoryFrame frame;
		std::string name;
	};
	// Source frames of a take cut from history, and where its replay began.
	struct Ghost
	{
		TakeSource source;
		// The predecessor belongs to the target run; earlier driven frames are not the replay.
		RuntimeFrameRun target;
		std::uint64_t afterFrame = 0, scriptRun = 0;
		std::optional<HistoryFrame> anchor;
	};
	void Play(RuntimePanels& runtime, const std::string& path);
	void DrawGhost(const HistoryStore& history);
	// The frame-rate meter in the LIVE column: numbers, and a sparkline when there is room.
	void DrawMeter(const HistoryStore& history, const RuntimeView& state, ShellRect area, bool oneRow);
	void DrawLegend();
	std::map<double, Marker> m_markers;
	std::optional<HistoryFrame> m_pendingMarker;
	std::array<char, 96> m_markerName{};
	TimelineHistory m_history;
	double m_cursorSeconds = 0;
	std::vector<TakeSource> m_takes;
	std::optional<Ghost> m_ghost;
	TimelineCanvas m_canvas;
	ShellRect m_drawer;
	bool m_tint = false;
};
} // namespace simxr::app
