#include "app/widgets/TimelinePanel.h"
#include "Pch.h"
#include "app/RuntimePanels.h"
#include "app/services/InputHistory.h"
#include "app/services/PlayerRig.h"
#include "app/services/RecordingController.h"
#include "app/services/Utf8Path.h"
#include "app/widgets/Controls.h"
#include "app/widgets/Theme.h"
#include <cstdio>
#include <imgui.h>

namespace simxr::app
{
static RuntimeFrameRun RunOf(const RuntimeView& state)
{
	return {state.epoch, state.session, state.generation};
}
void TimelinePanel::ObserveRig(const RuntimeView& runtime, std::uint64_t run, const automation::RigSnapshot& state,
							   bool matchesTarget)
{
	m_history.ObserveScript(runtime, run, state, matchesTarget);
}
void TimelinePanel::Observe(const RuntimeView& runtime)
{
	m_history.Observe(runtime, runtime.recording);
}
void TimelinePanel::SavedRecording(std::string path, const RuntimeView& runtime)
{
	m_history.SavedRecording(std::move(path), RunOf(runtime));
}
void TimelinePanel::Play(RuntimePanels& runtime, const std::string& path)
{
	try
	{
		runtime.RequestReplay(OpenSavedRecording(path));
	}
	catch (const std::exception& error)
	{
		m_history.Error(std::string("Cannot play take: ") + error.what(), runtime.View().historySeconds);
	}
}
void TimelinePanel::ReplayTake(const std::string& path, const RuntimeView& state, std::uint64_t scriptRun)
{
	m_history.ReplayTake(scriptRun, path, state);
	m_ghost.reset();
	for (const auto& take : m_takes)
		if (take.path == path)
			m_ghost = Ghost{take, RunOf(state), state.latestFrame, scriptRun};
}
void TimelinePanel::DrawGhost(const HistoryStore& history)
{
	if (!m_ghost)
		return;
	const auto* replay = m_history.FindScript(m_ghost->scriptRun);
	if (!replay || replay->first.Run() != m_ghost->target)
		return;
	const auto run = m_ghost->target;
	const auto origin = m_ghost->anchor ? history.Find(m_ghost->anchor->identity, run.epoch)
										: history.Find(replay->first.identity, run.epoch);
	const auto segment = origin ? std::optional{history.SegmentAt(*origin)}
								: history.LatestSegment({run.session, run.generation, 0}, run.epoch);
	if (!segment)
		return;
	const auto after = [&](std::uint64_t frame)
	{
		auto begin = segment->first, end = segment->second;
		while (begin < end)
		{
			const auto middle = begin + (end - begin) / 2;
			if (history.At(middle).identity.frame <= frame)
				begin = middle + 1;
			else
				end = middle;
		}
		return begin;
	};
	const auto end = after(replay->last.identity.frame);
	if (!m_ghost->anchor)
	{
		const auto first = after(m_ghost->afterFrame);
		if (first >= end)
			return;
		auto begin = first, limit = end;
		while (begin < limit)
		{
			const auto middle = begin + (limit - begin) / 2;
			if (history.Query(first, middle + 1).driven)
				limit = middle;
			else
				begin = middle + 1;
		}
		const auto& frame = history.At(begin < end ? begin : end - 1);
		const auto observed = (begin < end ? begin + 1 : end) - first;
		if (frame.identity.frame - m_ghost->afterFrame != observed)
		{
			m_ghost.reset();
			return;
		}
		m_ghost->afterFrame = frame.identity.frame;
		if (begin < end)
			m_ghost->anchor = frame;
	}
	if (!m_ghost->anchor)
		return;
	const auto& source = m_ghost->source;
	const auto first = history.Find(source.first, source.epoch), last = history.Find(source.last, source.epoch);
	const auto anchor = history.Find(m_ghost->anchor->identity, m_ghost->anchor->epoch);
	if (first && last && anchor && *last >= *first && *anchor < end)
		m_canvas.DrawGhost(history, *first, *last, *anchor, end - 1);
}
void TimelinePanel::DrawMeter(const HistoryStore& history, const RuntimeView& state, ShellRect area, bool oneRow)
{
	if (!state.sessionAvailable || area.width <= 0 || area.height <= 0)
		return;
	const auto segment = history.LatestSegment({state.session, state.generation, 0}, state.epoch);
	if (!segment)
		return;
	// The last 90 frames of this run, and every missed frame in the last 10 s.
	const auto last = segment->second - 1;
	const auto& newest = history.At(last);
	std::size_t first = last;
	float sum = 0, minimum = newest.frameMilliseconds, maximum = 0, cpuSum = 0, gpuSum = 0;
	std::size_t cpuCount = 0, gpuCount = 0;
	while (first > segment->first && last - first < 89)
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
	const auto missed =
		history.Query(std::max(segment->first, history.LowerBound(newest.seconds - 10)), last + 1).missed;
	// Green at budget, amber when frames run long, red when frames were missed.
	const ImU32 color = state.manual										   ? theme::TextDim
						: missed > 3 || (cpu >= 0 ? cpu : average) > budget	   ? theme::Record
						: missed || (cpu >= 0 ? cpu : average) >= budget * .9f ? theme::Warn
																			   : theme::Ok;
	char text[48];
	if (oneRow)
		std::snprintf(text, sizeof(text), "%.0f fps", average > 0 ? 1000 / average : 0);
	else
		std::snprintf(text, sizeof(text), "%.0f fps, %.1f ms", average > 0 ? 1000 / average : 0, average);
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
		char cpuText[48];
		std::snprintf(cpuText, sizeof(cpuText), "CPU span %.1f ms", cpu);
		draw->PushClipRect(hoverMin, hoverMax, true);
		draw->AddText({area.x, sparkTop}, theme::TextDim, cpuText);
		draw->PopClipRect();
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
							   : work >= budget * .9f						? theme::Warn
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
		ImGui::Text("Last %zu application intervals (ms): min %.1f / avg %.1f / max %.1f", count, minimum, average,
					maximum);
		ImGui::TextDisabled("Display period %.1f ms. %zu misses / 10 s", budget, missed);
		char gpuText[32] = "unavailable";
		if (gpu >= 0)
			std::snprintf(gpuText, sizeof(gpuText), "%.2f ms", gpu);
		if (cpu >= 0)
			ImGui::TextDisabled("CPU span %.1f ms. Compositor GPU %s", cpu, gpuText);
		else
			ImGui::TextDisabled("CPU span unavailable. Compositor GPU %s", gpuText);
		ImGui::TextDisabled("Application GPU time is unavailable");
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
	SectionLabel("Lanes");
	row("Frame time", "One bar per frame. Dashed line is the frame budget");
	row("", "Amber ran long, red missed its display time");
	row("paused", "The app was paused and advanced one frame at a time.");
	row("Images", "Saved images. Diamonds mark snapshots. Filled diamonds are pinned.");
	row("Input", "Left hand above, right hand below. Shading marks scripts and replays.");
	row("Red band", "Click the tab to play a take or open the Library.");
	row("Blue band", "A replay or script drove these frames");
	row("Ruler dots", "Connections, pauses and script output. Triangles mark saved points.");
	SectionLabel("Mouse and keys");
	row("Wheel", "Zoom around the pointer");
	row("Middle drag", "Pan");
	row("Click", "Show that frame");
	row("Drag ruler", "Select a range");
	row(", and .", "Go to the previous or next frame. Hold Shift to jump between markers.");
	row("M", "Add a marker");
	row("Home / End", "Fit everything / go live");
	ImGui::EndPopup();
}
bool TimelinePanel::Draw(RuntimePanels& runtime, ShellState& shell, bool folded)
{
	const auto& state = runtime.View();
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
	m_history.Resolve(history);
	if (auto saved = std::exchange(shell.timeline.savedTake, {}))
	{
		std::erase_if(m_takes,
					  [&](const TakeSource& take)
					  {
						  return take.path == saved->path;
					  });
		m_takes.push_back(std::move(*saved));
		if (m_takes.size() > 16)
			m_takes.erase(m_takes.begin());
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

	// Left column: transport, then the lane names the canvas draws below it, or the Library.
	const auto& style = ImGui::GetStyle();
	const float button = ImGui::GetFrameHeight(), scale = ImGui::GetFontSize() / 16;
	// Recording shows its elapsed time on the record button; room for it is kept so the lanes do not shift.
	const float clock = ImGui::CalcTextSize("00:00").x + style.FramePadding.x;
	const float transport = button * 7 + clock + style.ItemSpacing.x * 6;
	const bool drawer = shell.library && !folded;
	const float timelineWidth = ImGui::GetContentRegionAvail().x;
	const float header = std::max(transport + style.ItemSpacing.x * 2,
								  drawer ? std::clamp(timelineWidth * .32f, 260 * scale, 380 * scale)
										 : ImGui::CalcTextSize("Frame time").x + style.ItemSpacing.x * 2);
	const float liveWidth = std::max(ImGui::CalcTextSize("Go live").x + button + style.FramePadding.x,
									 ImGui::CalcTextSize("LIVE Frame 000000").x + 14 * scale) +
							button + style.ItemSpacing.x * 3 +
							(folded ? ImGui::CalcTextSize("000 fps").x + style.ItemSpacing.x : 0);
	const auto latest = history.Latest({state.session, state.generation, 0}, state.epoch);
	std::string roles;
	if (const auto index = state.time.following ? latest : cursorIndex)
		if (const auto* sample = runtime.InputSamples().Find(history.At(*index).input); sample && sample->trackers)
			for (const auto& tracker : *sample->trackers)
				if (tracker.connected)
				{
					roles += roles.empty() ? "Trackers: " : ", ";
					roles += TrackerRoleName(tracker.role);
				}
	m_canvas.SetInputDetail(std::move(roles));
	m_canvas.SetTint(m_tint);
	m_canvas.SetBudget(latest ? history.At(*latest).periodMilliseconds : 0);
	m_canvas.Begin(header, liveWidth, !drawer);
	const auto cell = m_canvas.TransportRect();
	m_drawer = drawer && !m_canvas.Compact()
				   ? ShellRect{cell.x, cell.y + cell.height, cell.width, m_canvas.Bounds().height - cell.height}
				   : ShellRect{};
	ImGui::SetCursorScreenPos({cell.x + style.ItemSpacing.x, cell.y + std::max(0.f, (cell.height - button) / 2)});
	ImGui::BeginGroup();
	const char* why = !state.sessionAvailable  ? "Connect an app first"
					  : state.operationPending ? "Waiting for the last command"
											   : nullptr;
	const bool take = shell.timeline.playing;
	// A playing take steps the paused app itself; Pause holds the take instead.
	const bool held = take ? shell.timeline.playbackPaused : state.manual;
	ImGui::BeginDisabled(!take && why != nullptr);
	const char* pauseTip = take	  ? (held ? "Continue the take" : "Pause the take")
						   : why  ? why
						   : held ? "Let the app run again (F8)"
								  : "Freeze the app on its current frame (F6)";
	if (held ? IconButton("Continue", Icon::Play, pauseTip) : IconButton("Pause", Icon::Pause, pauseTip))
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
	const auto current = state.time.following ? latest : cursorIndex;
	const bool atEdge = state.time.following || !current || *current + 1 == history.Size();
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
	const std::string forwardTip = std::string{shift		  ? "Next marker (Shift+.)"
											   : !atEdge	  ? "Next frame (.)"
											   : state.manual ? "Let the app make one more frame (. or F7)"
															  : "Pause first to step the app (F6)"} +
								   "\nRight-click for advanced stepping";
	if (IconButton("Step forward", Icon::StepForward, forwardTip.c_str()))
		stepForward();
	ImGui::EndDisabled();
	// Right-click offers the rarer stepping tools, even while stepping itself is unavailable.
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled) && ImGui::IsMouseReleased(ImGuiMouseButton_Right))
		ImGui::OpenPopup("Step options");
	if (ImGui::BeginPopup("Step options"))
	{
		if (ImGui::MenuItem("Advanced stepping"))
		{
			shell.selection = AdvancedSel{};
			inspect = true;
		}
		if (ImGui::MenuItem("Cancel pending", "Shift+F7"))
			runtime.CancelPending();
		ImGui::EndPopup();
	}
	ImGui::SameLine();
	ImGui::BeginDisabled(!state.sessionAvailable);
	char elapsed[32] = "";
	if (state.recording && shell.timeline.recordingSince)
	{
		const auto seconds = std::chrono::duration_cast<std::chrono::seconds>(std::chrono::steady_clock::now() -
																			  *shell.timeline.recordingSince)
								 .count();
		std::snprintf(elapsed, sizeof(elapsed), "%lld:%02lld", static_cast<long long>(seconds / 60),
					  static_cast<long long>(seconds % 60));
	}
	if (state.recording ? IconButton("Stop recording", Icon::Stop, "Stop recording and save the take (Ctrl+R)", false,
									 elapsed, theme::Record)
						: IconButton("Rec", Icon::Record, "Record inputs (Ctrl+R)", false, nullptr, theme::Record))
	{
		if (state.recording)
			runtime.StopRecording();
		else
			runtime.StartRecording();
	}
	ImGui::EndDisabled();
	ImGui::SameLine();
	if (IconButton("Library", Icon::Library, "Saved takes and scripts (Ctrl+L)", shell.library))
		shell.library = !shell.library;
	ImGui::SameLine();
	if (IconButton("Timeline help", Icon::Help, "What the lanes, marks and keys mean"))
		ImGui::OpenPopup("Timeline legend");
	ImGui::SameLine();
	if (IconButton("Fold timeline", folded ? Icon::ChevronUp : Icon::ChevronDown,
				   folded ? "Show the timeline (Ctrl+T)" : "Hide the timeline (Ctrl+T)"))
		shell.timeline.toggleFold = true;
	ImGui::EndGroup();
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
		if (IconButton("Go live", Icon::Live, "Jump to the newest frame (End)", false, roomy ? "Go live" : nullptr))
			goLive();
	}
	else if (state.sessionAvailable)
	{
		// Paused or running is the app control's dot; this says the view follows the newest frame, and which.
		const auto position = ImGui::GetCursorScreenPos();
		auto* draw = ImGui::GetWindowDrawList();
		const float radius = 3.5f * scale;
		draw->AddCircleFilled({position.x + radius, position.y + button / 2}, radius, theme::Ok);
		ImGui::SetCursorScreenPos({position.x + radius * 2 + 6 * scale, position.y});
		ImGui::AlignTextToFramePadding();
		ImGui::TextUnformatted("LIVE");
		if (roomy)
		{
			ImGui::SameLine(0, ImGui::CalcTextSize(" ").x);
			ImGui::TextDisabled("Frame %llu", static_cast<unsigned long long>(state.latestFrame));
		}
		ImGui::SetItemTooltip(state.manual ? "Step to show the next frame."
										   : "Showing the newest frame as the app makes it");
	}
	if (folded && roomy)
	{
		ImGui::SameLine();
		const auto position = ImGui::GetCursorScreenPos();
		DrawMeter(history, state, {position.x, live.y, ImGui::CalcTextSize("000 fps").x, live.height}, true);
	}
	if (roomy)
	{
		ImGui::SetCursorScreenPos({live.x + live.width - button - style.ItemSpacing.x, rowY});
		if (IconButton("Slow frames", Icon::Gauge,
					   m_tint ? "Stop highlighting missed frames" : "Highlight frames that missed their display time",
					   m_tint))
			m_tint = !m_tint;
	}
	if (!folded && roomy)
	{
		const auto bounds = m_canvas.Bounds();
		const float below = live.y + live.height;
		DrawMeter(history, state,
				  {live.x + style.ItemSpacing.x, below, live.width - style.ItemSpacing.x * 2,
				   bounds.y + bounds.height - below},
				  false);
	}
	if (state.historyFull)
	{
		const char* full = "History is full. Unpin a snapshot to free space.";
		const auto bounds = m_canvas.Bounds();
		ImGui::GetWindowDrawList()->AddText({bounds.x + bounds.width / 2 - ImGui::CalcTextSize(full).x / 2,
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
	const auto gesture =
		m_canvas.Draw(history, runtime.KeptPreviews(), runtime.PreviewCrop().value_or(ImVec4{0, 0, 1, 1}),
					  state.time.following, state.time.following ? std::optional<double>{} : cursor, selectedRange);
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
	for (const auto& pause : m_history.Pauses())
		m_canvas.DrawPause(pause.first.seconds, pause.last.seconds);
	if (const auto& pause = m_history.Pause())
		m_canvas.DrawPause(pause->first.seconds, pause->last.seconds);
	for (const auto& event : m_history.Events())
		m_canvas.DrawEvent(event.axisSeconds, event.seconds, event.text.c_str(), event.failed,
						   event.run == RuntimeFrameRun{});
	const auto clip = [&](const char* id, const TimelineSpan& span, bool running = false)
	{
		const auto name = span.take.empty() ? std::string(span.replay ? "Script" : "Recording")
											: PathText(Utf8Path(span.take).stem());
		const auto range = span.Range(history);
		const auto hit = m_canvas.DrawClip(id, span.first.seconds, span.last.seconds, name.c_str(), span.replay,
										   !span.take.empty() && runtime.CanReplay(), running);
		if (hit == ClipHit::Play)
			Play(runtime, span.take);
		else if (hit == ClipHit::Stop)
			shell.timeline.stopReplay = true;
		else if (hit == ClipHit::Opened)
		{
			if (span.take.empty())
				shell.library = true;
			else
			{
				shell.selection = TakeSel{span.take};
				inspect = true;
			}
		}
		else if (hit == ClipHit::Clicked && range)
		{
			choose(range->first);
			selectRange(range->first, range->second);
		}
	};
	for (const auto& span : m_history.Clips())
		clip(("Span/" + std::to_string(span.id)).c_str(), span);
	for (const auto& take : m_takes)
		if (const auto first = history.Find(take.first, take.epoch), last = history.Find(take.last, take.epoch);
			first && last)
		{
			const TimelineSpan span{{take.first, take.epoch, history.At(*first).seconds},
									{take.last, take.epoch, history.At(*last).seconds},
									0,
									false,
									take.path};
			const auto id = "Take/" + take.path + "/" + std::to_string(take.epoch) + "/" +
							std::to_string(take.first.session) + "/" + std::to_string(take.first.run) + "/" +
							std::to_string(take.first.frame) + "/" + std::to_string(take.last.session) + "/" +
							std::to_string(take.last.run) + "/" + std::to_string(take.last.frame);
			clip(id.c_str(), span);
		}
	if (const auto& span = m_history.Script())
		clip(("Span/" + std::to_string(span->id)).c_str(), *span, true);
	if (const auto& span = m_history.Recording())
		clip(("Span/" + std::to_string(span->id)).c_str(), *span);
	return inspect;
}
} // namespace simxr::app
