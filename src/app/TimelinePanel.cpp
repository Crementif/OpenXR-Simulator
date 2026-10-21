#include "app/TimelinePanel.h"
#include <algorithm>
#include <cmath>
#include <imgui.h>
#include <imgui_internal.h>
#include <limits>

namespace simxr::app
{
namespace
{
const char* Phase(automation::RigPhase phase)
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
} // namespace
void TimelinePanel::ObserveRig(const RuntimePanelSnapshot& runtime, std::uint64_t run,
							   const automation::RigSnapshot& state, bool matchesTarget)
{
	if (!run)
		return;
	const auto append = [&](std::string text, bool failed = false)
	{
		const RuntimeFrameRun scope =
			matchesTarget ? RuntimeFrameRun{runtime.epoch, runtime.session, runtime.generation} : RuntimeFrameRun{};
		m_events.push_back({scope, matchesTarget ? runtime.latestFrame : 0,
							std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count(),
							runtime.historySeconds, "Rig " + std::to_string(run) + ": " + std::move(text), failed});
		if (m_events.size() > 128)
			m_events.erase(m_events.begin());
	};
	if (run != m_rigRun)
	{
		m_rigRun = run;
		m_output = m_checkpoints = 0;
		append("Started");
		m_phase = automation::RigPhase::Idle;
	}
	if (state.phase != m_phase)
	{
		m_phase = state.phase;
		append(Phase(state.phase), state.phase == automation::RigPhase::Failed);
		if (state.phase == automation::RigPhase::Failed && !state.error.empty())
			append(state.error, true);
	}
	while (m_checkpoints < state.checkpoints.size())
	{
		const auto& point = state.checkpoints[m_checkpoints++];
		append(point.name + ": " + point.detail, !point.passed);
	}
	while (m_output < state.output.size())
		append(state.output[m_output++]);
}
bool TimelinePanel::Draw(RuntimePanels& runtime, ShellState& shell)
{
	const auto state = runtime.Snapshot();
	bool inspect = false;
	const auto& history = runtime.History();
	const auto choose = [&](std::size_t index)
	{
		const auto& frame = history.At(index);
		runtime.SelectHistoryFrame(index);
		shell.time.cursor = frame.identity;
		shell.selection = FrameSel{frame.identity};
		m_cursorSeconds = frame.seconds;
		inspect = true;
	};
	if (history.Size())
		std::erase_if(m_markers,
					  [&](const auto& marker)
					  {
						  return marker.seconds < history.At(0).seconds;
					  });
	ImGui::BeginDisabled(!state.sessionAvailable || state.operationPending);
	if (ImGui::Button(state.manual ? "Continue" : "Pause"))
	{
		if (state.manual)
			runtime.ResumeLive();
		else
		{
			runtime.Command(control::CommandKind::Pause);
			runtime.SetLive(false);
		}
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!state.sessionAvailable || !state.manual || state.operationPending);
	if (ImGui::Button("Next frame"))
	{
		runtime.Command(control::CommandKind::Step);
		shell.selection = FrameSel{shell.time.cursor};
		inspect = true;
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (ImGui::Button("Fit history"))
	{
		m_canvas.Fit();
		runtime.SetLive(false);
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!state.sessionAvailable || state.following);
	if (ImGui::Button("Show live"))
		runtime.SetLive(true);
	ImGui::EndDisabled();
	if (state.historyFull)
	{
		ImGui::SameLine();
		ImGui::TextColored({1, .65f, .3f, 1}, "History full — unpin snapshots");
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(m_markers.empty());
	const double cursor = state.following ? state.historySeconds : m_cursorSeconds;
	const auto jump = [&](bool next)
	{
		const HistoryFrame* target = nullptr;
		for (const auto& marker : m_markers)
			if (next ? marker.seconds > cursor : marker.seconds < cursor)
				if (!target || (next ? marker.seconds < target->seconds : marker.seconds > target->seconds))
					target = &marker;
		if (target)
			if (const auto index = history.Nearest(target->seconds))
				choose(*index);
	};
	if (ImGui::ArrowButton("Previous marker", ImGuiDir_Left))
		jump(false);
	ImGui::SameLine();
	if (ImGui::ArrowButton("Next marker", ImGuiDir_Right))
		jump(true);
	ImGui::EndDisabled();
	const auto gesture = m_canvas.Draw(runtime.History(), state.following);
	if (gesture.stopFollowing)
		runtime.SetLive(false);
	if (gesture.goLive)
		runtime.SetLive(true);
	if (gesture.first)
	{
		const auto& first = runtime.History().At(*gesture.first);
		choose(*gesture.first);
		if (gesture.last)
		{
			const auto& last = runtime.History().At(*gesture.last);
			if (first.epoch == last.epoch && first.identity.session == last.identity.session &&
				first.identity.run == last.identity.run)
				shell.selection = RangeSel{first.identity, last.identity};
		}
		inspect = true;
	}
	if (gesture.marker)
	{
		const auto& frame = history.At(*gesture.marker);
		const auto existing = std::find_if(m_markers.begin(), m_markers.end(),
										   [&](const auto& marker)
										   {
											   return marker.epoch == frame.epoch && marker.identity == frame.identity;
										   });
		if (existing != m_markers.end())
			m_markers.erase(existing);
		else
			m_markers.push_back(frame);
	}
	for (const auto& marker : m_markers)
		if (m_canvas.DrawMarker(marker.seconds, marker.identity.frame))
			if (const auto index = history.Nearest(marker.seconds))
				choose(*index);
	for (const auto& event : m_events)
		if (event.frame)
			m_canvas.DrawEvent(event.axisSeconds, event.seconds, event.text.c_str(), event.failed);
	return inspect;
}
} // namespace simxr::app
