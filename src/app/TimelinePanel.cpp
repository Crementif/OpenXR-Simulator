#include "app/TimelinePanel.h"
#include "app/RecordingController.h"
#include "app/Utf8Path.h"
#include "app/widgets/Controls.h"
#include "app/Theme.h"
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
void TimelinePanel::Play(RuntimePanels& runtime, const std::string& path)
{
	try
	{
		runtime.RequestReplay(OpenSavedRecording(path));
	}
	catch (const std::exception& error)
	{
		Push({{}, std::chrono::duration<double>(std::chrono::steady_clock::now() - m_started).count(),
			  m_previous ? m_previous->historySeconds : 0, std::string("Cannot play take: ") + error.what(), true});
	}
}
void TimelinePanel::ReplayTake(const std::string& path)
{
	m_ghost.reset();
	for (const auto& take : m_takes)
		if (take.path == path)
			m_ghost = Ghost{take, m_previous ? m_previous->historySeconds : 0};
}
void TimelinePanel::DrawGhost(const HistoryStore& history)
{
	if (!m_ghost)
		return;
	if (!m_ghost->begin && m_scriptBegin && *m_scriptBegin >= m_ghost->requested)
		m_ghost->begin = m_scriptBegin;
	// The take's initial values apply on the first frame the replay drives.
	if (m_ghost->begin && !m_ghost->anchor)
		for (auto index = history.LowerBound(*m_ghost->begin); index < history.Size(); ++index)
			if (history.At(index).flags & HistoryDriven)
			{
				m_ghost->anchor = history.At(index);
				break;
			}
	if (!m_ghost->anchor)
		return;
	const auto& source = m_ghost->source;
	const auto first = history.Find(source.first, source.epoch), last = history.Find(source.last, source.epoch);
	const auto anchor = history.Find(m_ghost->anchor->identity, m_ghost->anchor->epoch);
	if (first && last && anchor && *last >= *first)
		m_canvas.DrawGhost(history, *first, *anchor, *last - *first + 1);
}
void TimelinePanel::DrawMeter(const HistoryStore& history, const RuntimeView& state, ShellRect area, bool oneRow)
{
	if (!history.Size() || !state.sessionAvailable || area.width <= 0 || area.height <= 0)
		return;
	// The last 90 frames of this run, and every missed frame in the last 10 s.
	const auto last = history.Size() - 1;
	const auto& newest = history.At(last);
	std::size_t first = last;
	float sum = 0, minimum = newest.frameMilliseconds, maximum = 0, cpuSum = 0, gpuSum = 0;
	std::size_t cpuCount = 0, gpuCount = 0;
	while (first > 0 && last - first < 89 && !(history.At(first).flags & HistoryRunStart))
		--first;
	for (auto index = first; index <= last; ++index)
	{
		const float value = history.At(index).frameMilliseconds;
		sum += value;
		minimum = std::min(minimum, value);
		maximum = std::max(maximum, value);
		if (history.At(index).cpuMilliseconds >= 0)
		{
			cpuSum += history.At(index).cpuMilliseconds;
			++cpuCount;
		}
		if (history.At(index).compositorGpuMilliseconds >= 0)
		{
			gpuSum += history.At(index).compositorGpuMilliseconds;
			++gpuCount;
		}
	}
	const auto count = last - first + 1;
	const float average = sum / count, budget = newest.periodMilliseconds;
	const float cpu = cpuCount ? cpuSum / cpuCount : -1, gpu = gpuCount ? gpuSum / gpuCount : -1;
	std::size_t missed = 0;
	for (auto index = history.LowerBound(newest.seconds - 10); index <= last; ++index)
		missed += (history.At(index).flags & (HistoryLate | HistoryDropped)) != 0;
	// Green at budget, amber when frames run long, red when frames were missed.
	const ImU32 color = state.manual ? theme::TextDim
						: missed > 3 || (cpu >= 0 ? cpu : average) > budget ? theme::Record
						: missed || (cpu >= 0 ? cpu : average) >= budget * .9f ? theme::Warn
																 : theme::Ok;
	char text[48];
	if (oneRow)
		std::snprintf(text, sizeof(text), "%.0f fps", average > 0 ? 1000 / average : 0);
	else
		std::snprintf(text, sizeof(text), "%.0f fps \u00b7 %.1f ms", average > 0 ? 1000 / average : 0, average);
	auto* draw = ImGui::GetWindowDrawList();
	const float font = ImGui::GetFontSize(), scale = font / 16;
	const float textY = oneRow ? area.y + (area.height - font) / 2 : area.y + 2 * scale;
	draw->AddText({area.x, textY}, color, text);
	const ImVec2 hoverMin{area.x, area.y}, hoverMax{area.x + area.width, area.y + area.height};
	// A sparkline under the numbers: one bar per frame, dashed budget at mid height.
	float sparkTop = textY + font + 3 * scale;
	const float sparkBottom = area.y + area.height - 3 * scale;
	if (!oneRow && cpu >= 0 && sparkBottom - sparkTop >= font + 3 * scale)
	{
		char headroom[48];
		std::snprintf(headroom, sizeof(headroom), "CPU %+.1f ms spare", budget - cpu);
		draw->AddText({area.x, sparkTop}, color, headroom);
		sparkTop += font + 3 * scale;
	}
	if (!oneRow && sparkBottom - sparkTop >= 10 * scale)
	{
		const float height = sparkBottom - sparkTop, step = area.width / 90;
		for (auto index = first; index <= last; ++index)
		{
			const auto& frame = history.At(index);
			const float work = frame.cpuMilliseconds >= 0 ? frame.cpuMilliseconds : frame.frameMilliseconds;
			const float bar = std::min(height, height * work / (2 * std::max(1.f, budget)));
			const float x = area.x + (index - first) * step;
			const ImU32 tone = frame.flags & (HistoryLate | HistoryDropped) ? theme::Record
							   : work >= budget * .9f		 ? theme::Warn
																			 : theme::Accent;
			draw->AddRectFilled({x, sparkBottom - bar}, {x + std::max(1.f, step - scale), sparkBottom}, tone);
		}
		const float middle = sparkBottom - height / 2;
		for (float dash = area.x; dash < area.x + area.width; dash += 6 * scale)
			draw->AddLine({dash, middle}, {std::min(area.x + area.width, dash + 3 * scale), middle},
						  theme::Fade(theme::TextDim, .7f), scale);
	}
	if (ImGui::IsMouseHoveringRect(hoverMin, hoverMax) && ImGui::IsWindowHovered())
	{
		ImGui::BeginTooltip();
		ImGui::Text("Last %zu display intervals (ms): min %.1f / avg %.1f / max %.1f",
					count, minimum, average, maximum);
		char gpuText[32] = "unavailable";
		if (gpu >= 0)
			std::snprintf(gpuText, sizeof(gpuText), "%.2f ms", gpu);
		if (cpu >= 0)
			ImGui::TextDisabled("CPU %.1f ms / %+.1f ms spare; compositor GPU %s; %zu misses / 10 s",
				cpu, budget - cpu, gpuText, missed);
		else
			ImGui::TextDisabled("%.1f ms budget; CPU/GPU timing unavailable; %zu misses / 10 s", budget, missed);
		ImGui::EndTooltip();
	}
}
void TimelinePanel::DrawLegend()
{
	if (!ImGui::BeginPopup("Timeline legend"))
		return;
	const auto row = [](const char* name, const char* meaning)
	{
		ImGui::TextUnformatted(name);
		ImGui::SameLine(ImGui::GetFontSize() * 7);
		ImGui::TextDisabled("%s", meaning);
	};
	ImGui::SeparatorText("Lanes");
	row("Frame time", "One bar per frame; dashed line is the frame budget");
	row("", "Amber ran long, red missed its display time");
	row("paused", "The app was frozen; it only advanced when stepped");
	row("Images", "Kept images; diamonds are snapshots, filled when pinned");
	row("Input", "Left hand rises, right hand falls; shading is script or replay");
	row("Recording", "Recordings and replays; double-click opens the Library");
	row("Events", "Connects, pauses and script output");
	ImGui::SeparatorText("Mouse and keys");
	row("Wheel", "Zoom around the pointer");
	row("Middle drag", "Pan");
	row("Click", "Show that frame");
	row("Drag ruler", "Select a range");
	row(", and .", "Previous and next frame; with Shift, markers");
	row("M", "Add a marker");
	row("Home / End", "Fit everything / go live");
	ImGui::EndPopup();
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
bool TimelinePanel::Draw(RuntimePanels& runtime, ShellState& shell, bool folded, bool& fold)
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
	if (auto saved = std::exchange(shell.timeline.savedTake, {}))
	{
		std::erase_if(m_takes, [&](const TakeSource& take) { return take.path == saved->path; });
		m_takes.push_back(std::move(*saved));
		if (m_takes.size() > 16)
			m_takes.erase(m_takes.begin());
	}
	if (auto recorded = std::exchange(shell.timeline.recordedTake, {}))
		for (auto span = m_clips.rbegin(); span != m_clips.rend(); ++span)
			if (!span->replay)
			{
				if (span->take.empty())
					span->take = std::move(*recorded);
				break;
			}
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
	const float button = ImGui::GetFrameHeight();
	// Icon buttons with a word are one frame height wide plus the word.
	const auto buttonWidth = [&](const char* label)
	{
		return button + ImGui::CalcTextSize(label).x + style.FramePadding.x;
	};
	const bool transportWords = ImGui::GetContentRegionAvail().x >= 720 * ImGui::GetFontSize() / 16;
	const float transport = (transportWords ? buttonWidth("Continue") : button) +
		button * 4 + style.ItemSpacing.x * 4;
	const float header = std::max(transport, ImGui::CalcTextSize("Frame time").x) + style.ItemSpacing.x * 2;
	const float liveWidth = std::max(buttonWidth("Go live"), ImGui::CalcTextSize("LIVE \u00b7 paused").x + button) +
							button * 2 + style.ItemSpacing.x * 4 +
							(folded ? ImGui::CalcTextSize("000 fps").x + style.ItemSpacing.x : 0);
	m_canvas.SetTint(m_tint);
	if (history.Size())
		m_canvas.SetBudget(history.At(history.Size() - 1).periodMilliseconds);
	m_canvas.Begin(header, liveWidth);
	const auto cell = m_canvas.TransportRect();
	ImGui::SetCursorScreenPos(
		{cell.x + style.ItemSpacing.x, cell.y + std::max(0.f, (cell.height - button) / 2)});
	const char* why = !state.sessionAvailable ? "Connect an app first"
					  : state.operationPending ? "Waiting for the last command"
											   : nullptr;
	const bool take = shell.timeline.playing;
	// A playing take steps the paused app itself; Pause holds the take instead.
	const bool held = take ? shell.timeline.playbackPaused : state.manual;
	ImGui::BeginDisabled(!take && why != nullptr);
	const char* pauseTip = take ? (held ? "Continue the take" : "Pause the take")
					   : why	? why
					   : held	? "Let the app run again (F8)"
								: "Freeze the app on its current frame (F6)";
	if (held ? IconButton("Continue", Icon::Play, pauseTip, false, transportWords ? "Continue" : nullptr)
			 : IconButton("Pause", Icon::Pause, pauseTip, false, transportWords ? "Pause" : nullptr))
	{
		if (take)
			shell.timeline.togglePlayback = true;
		else if (state.manual)
		{
			runtime.ResumeLive();
			LeaveHistory(shell, false);
		}
		else
			runtime.Pause();
	}
	ImGui::EndDisabled();
	// The arrows step through frames; at the paused live edge the next frame is
	// made by the app. Shift jumps between markers instead.
	const std::optional<std::size_t> latest =
		history.Size() ? std::optional<std::size_t>(history.Size() - 1) : std::nullopt;
	const auto current = state.time.following ? latest : cursorIndex;
	const bool atEdge = !current || *current == *latest;
	const bool shift = ImGui::GetIO().KeyShift;
	const auto stepBack = [&]
	{
		if (shift)
			jump(false);
		else if (current && *current > 0)
			choose(*current - 1);
	};
	const auto stepForward = [&]
	{
		if (shift)
			jump(true);
		else if (!atEdge)
			choose(*current + 1);
		else if (state.manual && !why)
		{
			runtime.Step();
			inspect = true;
		}
		else if (!state.time.following)
			goLive();
	};
	ImGui::SameLine();
	ImGui::BeginDisabled(!shift && (!current || *current == 0));
	if (IconButton("Step back", Icon::StepBack, shift ? "Previous marker (Shift+,)" : "Previous frame (,)"))
		stepBack();
	ImGui::EndDisabled();
	ImGui::SameLine();
	const bool canStep = shift || !atEdge || (state.manual && !why) || !state.time.following;
	ImGui::BeginDisabled(!canStep);
	const char* forwardTip = shift			  ? "Next marker (Shift+.)"
							 : !atEdge		  ? "Next frame (.)"
							 : state.manual	  ? "Let the app make one more frame (. or F7)"
											  : "Pause first to step the app (F6)";
	if (IconButton("Step forward", Icon::StepForward, forwardTip))
		stepForward();
	ImGui::EndDisabled();
	ImGui::SameLine();
	ImGui::BeginDisabled(!state.sessionAvailable);
	if (state.recording ? IconButton("Stop recording", Icon::Stop, "Stop recording and save the take (Ctrl+R)",
									 false, nullptr, theme::Record)
						: IconButton("Rec", Icon::Record, "Record inputs (Ctrl+R)", false, nullptr, theme::Record))
	{
		if (state.recording)
			runtime.StopRecording();
		else
			runtime.StartRecording();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (IconButton("Timeline help", Icon::Help, "What the lanes, marks and keys mean"))
		ImGui::OpenPopup("Timeline legend");
	DrawLegend();
	if (!ImGui::GetIO().WantTextInput && ImGui::IsWindowHovered(ImGuiHoveredFlags_ChildWindows))
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Comma))
			stepBack();
		if (ImGui::IsKeyPressed(ImGuiKey_Period))
			stepForward();
	}
	// The LIVE column right of the lanes: where the view is, the frame rate,
	// the slow-frame tint and the fold chevron.
	const auto live = m_canvas.LiveRect();
	// A narrow window keeps only LIVE and the chevron in this column.
	const bool roomy = live.width >= liveWidth - 1;
	const float rowY = live.y + std::max(0.f, (live.height - button) / 2);
	ImGui::SetCursorScreenPos({live.x + style.ItemSpacing.x, rowY});
	if (!state.time.following && state.sessionAvailable)
	{
		if (IconButton("Go live", Icon::Live, "Jump to the newest frame (End)", false, "Go live"))
			goLive();
	}
	else if (state.sessionAvailable)
		StateChip("Live state", state.manual ? theme::Paused : theme::Ok, state.manual ? "LIVE \u00b7 paused" : "LIVE",
				  state.manual ? "Showing the paused frame; stepping shows each new frame"
							   : "Showing the newest frame as the app makes it");
	if (folded && roomy)
	{
		ImGui::SameLine();
		const auto position = ImGui::GetCursorScreenPos();
		DrawMeter(history, state, {position.x, live.y, ImGui::CalcTextSize("000 fps").x, live.height}, true);
	}
	ImGui::SetCursorScreenPos({live.x + live.width - button * (roomy ? 2 : 1) - style.ItemSpacing.x * 2, rowY});
	if (roomy)
	{
		if (IconButton("Slow frames", Icon::Gauge,
					   m_tint ? "Stop highlighting missed frames" : "Highlight frames that missed their display time",
					   m_tint))
			m_tint = !m_tint;
		ImGui::SameLine();
	}
	if (IconButton("Fold timeline", folded ? Icon::ChevronUp : Icon::ChevronDown,
				   folded ? "Show the timeline (Ctrl+T)" : "Hide the timeline (Ctrl+T)"))
		fold = true;
	if (!folded && roomy)
	{
		const auto bounds = m_canvas.Bounds();
		const float below = live.y + live.height;
		DrawMeter(history, state, {live.x + style.ItemSpacing.x, below, live.width - style.ItemSpacing.x * 2,
								   bounds.y + bounds.height - below},
				  false);
	}
	if (state.historyFull)
	{
		const char* full = "History full — unpin snapshots";
		const auto bounds = m_canvas.Bounds();
		ImGui::GetWindowDrawList()->AddText(
			{bounds.x + bounds.width / 2 - ImGui::CalcTextSize(full).x / 2,
			 cell.y + (cell.height - ImGui::GetFontSize()) / 2},
			theme::Paused, full);
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
	const auto gesture = m_canvas.Draw(history, runtime.KeptPreviews(),
		 runtime.PreviewCrop().value_or(ImVec4{0, 0, 1, 1}), state.time.following,
		 state.time.following ? std::optional<double>{} : cursor, selectedRange);
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
	DrawGhost(history);
	for (const auto& pause : m_pauses)
		m_canvas.DrawPause(pause.first, pause.last);
	if (m_pauseBegin)
		m_canvas.DrawPause(*m_pauseBegin, state.historySeconds);
	for (const auto& event : m_events)
		m_canvas.DrawEvent(event.axisSeconds, event.seconds, event.text.c_str(), event.failed,
						   event.run == RuntimeFrameRun{});
	const auto clip = [&](double first, double last, bool replay, const std::string& take = {})
	{
		const auto name = take.empty() ? std::string(replay ? (shell.timeline.playing ? "Take" : "Script") : "Recording")
									   : PathText(Utf8Path(take).stem());
		const auto hit = m_canvas.DrawClip(first, last, name.c_str(), replay, !take.empty() && runtime.CanReplay());
		if (hit == ClipHit::Play)
			Play(runtime, take);
		else if (hit == ClipHit::Opened)
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
		clip(span.first, span.last, span.replay, span.take);
	// Takes cut from history sit over their source frames.
	for (const auto& take : m_takes)
		if (take.epoch == state.epoch)
			if (const auto a = history.Find(take.first, take.epoch), b = history.Find(take.last, take.epoch); a && b)
				clip(history.At(*a).seconds, history.At(*b).seconds, false, take.path);
	if (m_scriptBegin)
		clip(*m_scriptBegin, state.historySeconds, true);
	if (m_recordingBegin)
		clip(*m_recordingBegin, state.historySeconds, false);
	return inspect;
}
} // namespace simxr::app
