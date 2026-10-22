#include "app/TimelinePanel.h"
#include <algorithm>
#include <cstdio>
#include <imgui.h>
#include <utility>

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
template <typename T> void Bounded(std::vector<T>& values)
{
	if (values.size() > 128)
		values.erase(values.begin());
}
RuntimeFrameRun RunOf(const RuntimeView& state)
{
	return {state.epoch, state.session, state.generation};
}
} // namespace
void TimelinePanel::Push(Event event)
{
	m_events.push_back(std::move(event));
	Bounded(m_events);
}
void TimelinePanel::ObserveRig(const RuntimeView& runtime, std::uint64_t run,
							   const automation::RigSnapshot& state, bool matchesTarget)
{
	if (!run)
		return;
	const auto append = [&](std::string text, bool failed = false)
	{
		Push({matchesTarget ? RunOf(runtime) : RuntimeFrameRun{},
			  std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count(),
			  runtime.historySeconds, "Script " + std::to_string(run) + ": " + std::move(text), failed});
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
		if (matchesTarget && state.phase == automation::RigPhase::Running && !m_scriptBegin)
			m_scriptBegin = runtime.historySeconds;
		if (m_scriptBegin &&
			(state.phase == automation::RigPhase::Complete || state.phase == automation::RigPhase::Failed ||
			 state.phase == automation::RigPhase::Cancelled))
		{
			m_clips.push_back({*m_scriptBegin, runtime.historySeconds, true});
			Bounded(m_clips);
			m_scriptBegin.reset();
		}
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
void TimelinePanel::Observe(const RuntimeView& state)
{
	const auto observed = [&](const char* text, const RuntimeView& at)
	{
		Push({RunOf(at), std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count(),
			  at.historySeconds, text, false});
	};
	const auto closeSpans = [&](double seconds)
	{
		if (m_recordingBegin)
			m_clips.push_back({*std::exchange(m_recordingBegin, {}), seconds, false});
		if (m_scriptBegin)
			m_clips.push_back({*std::exchange(m_scriptBegin, {}), seconds, true});
		if (m_pauseBegin)
			m_pauses.push_back({*std::exchange(m_pauseBegin, {}), seconds});
		Bounded(m_clips);
		Bounded(m_pauses);
	};
	if (m_previous)
	{
		// A run ends at its last retained frame; nothing open spans into the next run.
		if (m_previous->connected && (!state.connected || RunOf(state) != RunOf(*m_previous)))
		{
			observed("Disconnected", *m_previous);
			closeSpans(m_previous->historySeconds);
			m_pauseEnd.reset();
		}
		if (state.connected && (!m_previous->connected || RunOf(state) != RunOf(*m_previous)))
			observed("Connected", state);
		else if (state.connected && state.manual != m_previous->manual)
			observed(state.manual ? "Paused" : "Continued", state);
	}
	if (state.recording && !m_recordingBegin)
	{
		m_recordingBegin = state.historySeconds;
		observed("Recording started", state);
	}
	if (!state.recording && m_recordingBegin)
	{
		m_clips.push_back({*std::exchange(m_recordingBegin, {}), state.historySeconds, false});
		Bounded(m_clips);
		observed("Recording stopped", state);
	}
	if (state.connected && state.manual && !m_pauseBegin)
	{
		m_pauseBegin = state.historySeconds;
		m_pauseFrame = HistoryFrame{{state.session, state.generation, state.latestFrame}, state.epoch};
	}
	if (!state.manual && m_pauseBegin)
	{
		m_pauses.push_back({*std::exchange(m_pauseBegin, {}), state.historySeconds});
		Bounded(m_pauses);
		m_pauseEnd = state.historySeconds;
	}
	// The pause lasts until the first frame after Continue.
	if (m_pauseEnd && state.historySeconds > *m_pauseEnd)
	{
		m_pauses.back().last = state.historySeconds;
		m_pauseEnd.reset();
	}
	m_previous = state;
}
bool TimelinePanel::Draw(RuntimePanels& runtime, ShellState& shell)
{
	const auto state = runtime.Snapshot();
	const auto& history = runtime.History();
	bool inspect = false;
	const auto choose = [&](std::size_t index)
	{
		const auto& frame = history.At(index);
		runtime.LookAt(index);
		shell.selection = FrameSel{frame.identity, frame.epoch};
		m_cursorSeconds = frame.seconds;
		inspect = true;
	};
	const auto goLive = [&]
	{
		runtime.SetLive(true);
		LeaveHistory(shell, true);
	};
	Observe(state);
	// A pause starts at the frame it stopped on, which history learns about a little later.
	if (m_pauseBegin && m_pauseFrame)
		if (const auto paused = history.Find(m_pauseFrame->identity, m_pauseFrame->epoch))
		{
			m_pauseBegin = std::min(*m_pauseBegin, history.At(*paused).seconds);
			m_pauseFrame.reset();
		}
	if (history.Size())
		while (!m_markers.empty() && m_markers.begin()->first < history.At(0).seconds)
			m_markers.erase(m_markers.begin());
	const auto cursorIndex = history.Find(state.time.cursor, state.time.epoch);
	if (cursorIndex)
		m_cursorSeconds = history.At(*cursorIndex).seconds;
	const double cursor = state.time.following ? state.historySeconds : m_cursorSeconds;
	const auto jump = [&](bool next)
	{
		auto marker = next ? m_markers.upper_bound(cursor) : m_markers.lower_bound(cursor);
		if (!next)
		{
			if (marker == m_markers.begin())
				return;
			--marker;
		}
		if (marker != m_markers.end())
			if (const auto index = history.Find(marker->second.frame.identity, marker->second.frame.epoch))
				choose(*index);
	};

	// Left column: transport, then the lane names the canvas draws below it.
	const auto& style = ImGui::GetStyle();
	const auto buttonWidth = [&](const char* label)
	{
		return ImGui::CalcTextSize(label).x + style.FramePadding.x * 2;
	};
	const float arrows = ImGui::GetFrameHeight() * 2 + style.ItemSpacing.x;
	const float transport = buttonWidth("Continue") + buttonWidth("Next") + arrows + style.ItemSpacing.x * 2;
	const float header = std::max(transport, ImGui::CalcTextSize("Recording").x) + style.ItemSpacing.x * 2;
	const float liveWidth = buttonWidth("LIVE >|") + style.ItemSpacing.x * 2;
	m_canvas.Begin(header, liveWidth);
	const auto cell = m_canvas.TransportRect();
	ImGui::SetCursorScreenPos(
		{cell.x + style.ItemSpacing.x, cell.y + std::max(0.f, (cell.height - ImGui::GetFrameHeight()) / 2)});
	const char* why = !state.sessionAvailable ? "Connect an app first"
					  : state.operationPending ? "Waiting for the last command"
											   : nullptr;
	ImGui::BeginDisabled(why != nullptr);
	if (state.manual ? ImGui::Button("Continue") : ImGui::Button("Pause"))
	{
		if (state.manual)
		{
			runtime.ResumeLive();
			LeaveHistory(shell, false);
		}
		else
			runtime.Pause();
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("%s", why ? why : state.manual ? "Continue (F8)" : "Pause (F6)");
	ImGui::SameLine();
	ImGui::BeginDisabled(why != nullptr || !state.manual);
	if (ImGui::Button("Next"))
	{
		runtime.Step();
		inspect = true;
	}
	ImGui::EndDisabled();
	ImGui::SetItemTooltip("%s", why ? why : !state.manual ? "Next frame (F7) — pause first" : "Next frame (F7)");
	ImGui::SameLine();
	ImGui::BeginDisabled(m_markers.empty());
	if (ImGui::ArrowButton("Previous marker", ImGuiDir_Left))
		jump(false);
	ImGui::SetItemTooltip("Previous marker (,)");
	ImGui::SameLine();
	if (ImGui::ArrowButton("Next marker", ImGuiDir_Right))
		jump(true);
	ImGui::SetItemTooltip(m_markers.empty() ? "Press M over the timeline to add a marker" : "Next marker (.)");
	ImGui::EndDisabled();
	if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Comma))
			jump(false);
		if (ImGui::IsKeyPressed(ImGuiKey_Period))
			jump(true);
	}
	// LIVE has its own column right of the lanes: a label while following, the way back otherwise.
	const auto live = m_canvas.LiveRect();
	ImGui::SetCursorScreenPos(
		{live.x + style.ItemSpacing.x, live.y + std::max(0.f, (live.height - ImGui::GetFrameHeight()) / 2)});
	if (!state.time.following && state.sessionAvailable)
	{
		if (ImGui::Button("LIVE >|"))
			goLive();
		ImGui::SetItemTooltip("Back to the newest frame (End)");
	}
	else if (state.sessionAvailable)
	{
		ImGui::AlignTextToFramePadding();
		ImGui::TextColored({0.35f, 0.80f, 0.65f, 1}, "LIVE");
		ImGui::SetItemTooltip(state.manual ? "Following the paused frame" : "Following the newest frame");
	}
	if (state.historyFull)
	{
		const char* full = "History full — unpin snapshots";
		const auto bounds = m_canvas.Bounds();
		ImGui::GetWindowDrawList()->AddText(
			{bounds.x + bounds.width / 2 - ImGui::CalcTextSize(full).x / 2,
			 cell.y + (cell.height - ImGui::GetFontSize()) / 2},
			IM_COL32(240, 164, 58, 255), full);
	}

	if (shell.timeline.zoomToRange)
	{
		m_canvas.Zoom(shell.timeline.first, shell.timeline.last);
		shell.timeline.zoomToRange = false;
		runtime.SetLive(false);
	}
	std::optional<std::pair<double, double>> selectedRange;
	if (const auto* range = std::get_if<RangeSel>(&shell.selection))
	{
		const auto first = history.Find(range->first, range->epoch), last = history.Find(range->last, range->epoch);
		if (first && last)
			selectedRange = {history.At(*first).seconds, history.At(*last).seconds};
	}
	const auto gesture =
		m_canvas.Draw(history, state.time.following, state.time.following ? std::optional<double>{} : cursor, selectedRange);
	shell.timelineCanvas = m_canvas.Bounds();
	std::tie(shell.timeline.first, shell.timeline.last) = m_canvas.VisibleRange();
	if (gesture.stopFollowing)
		runtime.SetLive(false);
	if (gesture.goLive)
		goLive();
	if (gesture.pin)
		runtime.TogglePin(*gesture.pin);
	const auto selectRange = [&](std::size_t firstIndex, std::size_t lastIndex)
	{
		const auto &first = history.At(firstIndex), &last = history.At(lastIndex);
		if (first.epoch == last.epoch && first.identity.session == last.identity.session &&
			first.identity.run == last.identity.run)
			shell.selection = RangeSel{first.identity, last.identity, first.epoch};
	};
	if (gesture.first)
	{
		choose(*gesture.first);
		if (gesture.last)
			selectRange(*gesture.first, *gesture.last);
	}
	if (gesture.marker)
	{
		m_pendingMarker = history.At(*gesture.marker);
		const auto existing = m_markers.find(m_pendingMarker->seconds);
		std::snprintf(m_markerName.data(), m_markerName.size(), "%s",
					  existing == m_markers.end() ? "Marker" : existing->second.name.c_str());
		ImGui::OpenPopup("Marker");
	}
	if (ImGui::BeginPopup("Marker"))
	{
		ImGui::InputText("Name", m_markerName.data(), m_markerName.size());
		if (m_pendingMarker)
		{
			const auto index = history.Find(m_pendingMarker->identity, m_pendingMarker->epoch);
			ImGui::BeginDisabled(!index || !m_markerName[0]);
			if (ImGui::Button("Save marker"))
			{
				m_markers.insert_or_assign(m_pendingMarker->seconds, Marker{*m_pendingMarker, m_markerName.data()});
				runtime.MarkHistoryFrame(*index, true);
				ImGui::CloseCurrentPopup();
			}
			ImGui::EndDisabled();
			if (m_markers.contains(m_pendingMarker->seconds))
			{
				ImGui::SameLine();
				if (ImGui::Button("Delete"))
				{
					m_markers.erase(m_pendingMarker->seconds);
					if (index)
						runtime.MarkHistoryFrame(*index, false);
					ImGui::CloseCurrentPopup();
				}
			}
		}
		ImGui::EndPopup();
	}
	if (gesture.hovered)
		if (const auto marker = m_markers.find(history.At(*gesture.hovered).seconds); marker != m_markers.end())
			ImGui::SetTooltip("%s / frame %llu", marker->second.name.c_str(),
							  static_cast<unsigned long long>(marker->second.frame.identity.frame));
	for (const auto& pause : m_pauses)
		m_canvas.DrawPause(pause.first, pause.last);
	if (m_pauseBegin)
		m_canvas.DrawPause(*m_pauseBegin, state.historySeconds);
	for (const auto& event : m_events)
		m_canvas.DrawEvent(event.axisSeconds, event.seconds, event.text.c_str(), event.failed,
						   event.run == RuntimeFrameRun{});
	const auto clip = [&](double first, double last, bool replay)
	{
		const auto hit = m_canvas.DrawClip(first, last, replay ? "Script" : "Recording", replay);
		if (hit == ClipHit::Opened)
		{
			shell.selection = LibrarySel{};
			inspect = true;
		}
		else if (hit == ClipHit::Clicked && history.Size())
			if (const auto a = history.Nearest(first), b = history.Nearest(last); a && b)
			{
				choose(*a);
				selectRange(*a, *b);
			}
	};
	for (const auto& span : m_clips)
		clip(span.first, span.last, span.replay);
	if (m_scriptBegin)
		clip(*m_scriptBegin, state.historySeconds, true);
	if (m_recordingBegin)
		clip(*m_recordingBegin, state.historySeconds, false);
	return inspect;
}
} // namespace simxr::app
