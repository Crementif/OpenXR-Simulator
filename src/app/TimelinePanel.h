#pragma once

#include "app/RuntimePanels.h"
#include "automation/RigRunner.h"
#include <chrono>
#include <string>
#include <vector>

namespace simxr::app
{
// Retained-frame navigation and bounded, explicitly observed rig events.
class TimelinePanel
{
  public:
	bool Draw(RuntimePanels& runtime);
	void ObserveRig(const RuntimePanelSnapshot& runtime, std::uint64_t run, const automation::RigSnapshot& state,
					bool matchesTarget);

  private:
	struct Event
	{
		RuntimeFrameRun run;
		std::uint64_t frame = 0;
		double seconds = 0;
		std::string text;
		bool failed = false;
	};
	RuntimeFrameRun m_run, m_currentRun;
	std::vector<Event> m_events;
	std::chrono::steady_clock::time_point m_started = std::chrono::steady_clock::now();
	std::uint64_t m_rigRun = 0, m_origin = 0;
	automation::RigPhase m_phase = automation::RigPhase::Idle;
	std::size_t m_output = 0, m_checkpoints = 0;
	bool m_fit = true;
	double m_start = 0, m_span = 1;
	void DrawEvents();
};
} // namespace simxr::app
