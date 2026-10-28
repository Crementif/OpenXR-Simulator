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
class TimelinePanel
{
  public:
	bool Draw(RuntimePanels& runtime, ShellState& shell, bool folded);
	static float FoldedHeight(float scale)
	{
		return 36 * scale;
	}
	ShellRect DrawerRect() const
	{
		return m_drawer;
	}
	void ObserveRig(const RuntimeView& runtime, std::uint64_t run, const automation::RigSnapshot& state,
					bool matchesTarget);
	void Observe(const RuntimeView& runtime);
	void SavedRecording(std::string path, const RuntimeView& runtime);
	void ReplayTake(const std::string& path, const RuntimeView& state, std::uint64_t scriptRun);

  private:
	struct Marker
	{
		HistoryFrame frame;
		std::string name;
	};
	struct Ghost
	{
		TakeSource source;
		RuntimeFrameRun target;
		std::uint64_t afterFrame = 0, scriptRun = 0;
		std::optional<HistoryFrame> anchor;
	};
	void Play(RuntimePanels& runtime, const std::string& path);
	void DrawGhost(const HistoryStore& history);
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
