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
double Offset(std::uint64_t frame, std::uint64_t origin)
{
	return frame >= origin ? double(frame - origin) : -double(origin - frame);
}
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
std::string RunLabel(RuntimeFrameRun run)
{
	return "Session " + std::to_string(run.session) + " / run " + std::to_string(run.generation) + " / connection " +
		   std::to_string(run.epoch);
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
							"Rig " + std::to_string(run) + ": " + std::move(text), failed});
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
void TimelinePanel::DrawEvents()
{
	if (ImGui::Button("Rig events"))
		ImGui::OpenPopup("Timeline rig events");
	ImGui::SetNextWindowSizeConstraints(
		{260, 0}, {ImGui::GetMainViewport()->WorkSize.x * .9f, ImGui::GetMainViewport()->WorkSize.y * .7f});
	if (ImGui::BeginPopup("Timeline rig events"))
	{
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + ImGui::GetFontSize() * 30);
		ImGui::TextWrapped("Last 128 events. Times are UI observations since this companion started; frame positions "
						   "use the last observed submitted XR frame, not exact event timestamps.");
		if (m_events.empty())
			ImGui::TextUnformatted("Run a recording or rig to see activity here.");
		for (auto it = m_events.rbegin(); it != m_events.rend(); ++it)
		{
			ImGui::Separator();
			if (it->frame)
				ImGui::TextDisabled("+%.2fs / observed near frame %llu", it->seconds,
									static_cast<unsigned long long>(it->frame));
			else
				ImGui::TextDisabled("+%.2fs / no frame association", it->seconds);
			ImGui::TextWrapped("%s", it->text.c_str());
		}
		ImGui::PopTextWrapPos();
		ImGui::EndPopup();
	}
}
bool TimelinePanel::Draw(RuntimePanels& runtime)
{
	const auto state = runtime.Snapshot();
	auto frames = runtime.TimelineFrames();
	const RuntimeFrameRun current{state.epoch, state.session, state.generation};
	if (state.sessionAvailable && current != m_currentRun)
	{
		m_run = m_currentRun = current;
		m_fit = true;
	}
	if (!m_run.session && !frames.empty())
		m_run = frames.back().run;
	std::vector<RuntimeFrameRun> runs;
	if (state.sessionAvailable)
		runs.push_back(current);
	for (const auto& frame : frames)
		if (std::find(runs.begin(), runs.end(), frame.run) == runs.end())
			runs.push_back(frame.run);
	const auto viewport = ImGui::GetMainViewport();
	const float font = ImGui::GetFontSize(), row = ImGui::GetFrameHeightWithSpacing();
	const bool compact = viewport->WorkSize.x < font * 74;
	const float height = row * (compact ? 2 : 1) + font * 2.5f + ImGui::GetStyle().WindowPadding.y * 2;
	if (!ImGui::BeginViewportSideBar("##Frame timeline", viewport, ImGuiDir_Down, height,
									 ImGuiWindowFlags_NoScrollbar | ImGuiWindowFlags_NoSavedSettings))
	{
		ImGui::End();
		return false;
	}
	bool inspect = false;
	ImGui::BeginDisabled(!state.sessionAvailable || state.operationPending);
	if (ImGui::Button(state.manual ? "Continue" : "Pause"))
	{
		if (state.manual)
			runtime.ResumeLive();
		else
			runtime.Command(control::CommandKind::Pause);
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Pause application execution (F6), or continue and follow live output (F8).");
	ImGui::SameLine();
	ImGui::BeginDisabled(!state.sessionAvailable || !state.manual || state.operationPending);
	if (ImGui::Button("Next frame"))
	{
		runtime.Command(control::CommandKind::Step);
		inspect = true;
	}
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Advance one application frame and capture it (F7).");
	ImGui::SameLine();
	ImGui::BeginDisabled(!state.sessionAvailable || state.following);
	if (ImGui::Button("Show live"))
		runtime.SetLive(true);
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Follow live images; application execution is unchanged.");
	if (!compact)
		ImGui::SameLine();
	ImGui::SetNextItemWidth(font * 12);
	const auto caption = m_run.session
							 ? "Run " + std::to_string(m_run.generation) + " / session " + std::to_string(m_run.session)
							 : "No snapshots yet";
	if (ImGui::BeginCombo("##Timeline run", caption.c_str()))
	{
		for (const auto& run : runs)
			if (ImGui::Selectable(RunLabel(run).c_str(), run == m_run))
			{
				m_run = run;
				m_fit = true;
			}
		ImGui::EndCombo();
	}
	ImGui::SameLine();
	if (ImGui::Button("Fit history"))
		m_fit = true;
	ImGui::SameLine();
	DrawEvents();
	std::erase_if(frames,
				  [&](const auto& frame)
				  {
					  return frame.run != m_run;
				  });
	std::sort(frames.begin(), frames.end(),
			  [](const auto& a, const auto& b)
			  {
				  return a.frame == b.frame ? a.local < b.local : a.frame < b.frame;
			  });
	const auto selected = std::find_if(frames.begin(), frames.end(),
									   [](const auto& frame)
									   {
										   return frame.selected;
									   });
	const auto choose = [&](const RuntimeFrame& frame)
	{
		runtime.SelectFrame(frame.local);
		inspect = true;
	};
	ImGui::SameLine();
	ImGui::BeginDisabled(frames.empty() || (selected != frames.end() && selected == frames.begin()));
	if (ImGui::ArrowButton("Previous snapshot", ImGuiDir_Left))
		choose(selected == frames.end() ? frames.back() : *std::prev(selected));
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Inspect previous retained snapshot; does not rewind execution.");
	ImGui::SameLine();
	ImGui::BeginDisabled(frames.empty() || selected == frames.end() || std::next(selected) == frames.end());
	if (ImGui::ArrowButton("Next snapshot", ImGuiDir_Right))
		choose(*std::next(selected));
	ImGui::EndDisabled();
	if (ImGui::IsItemHovered(ImGuiHoveredFlags_AllowWhenDisabled))
		ImGui::SetTooltip("Inspect next retained snapshot; does not advance execution.");
	if (ImGui::GetContentRegionAvail().x > font * 14)
	{
		ImGui::SameLine();
		if (selected != frames.end())
			ImGui::TextDisabled("Frame %llu", static_cast<unsigned long long>(selected->frame));
		else
			ImGui::TextDisabled("%zu snapshots", frames.size());
	}

	const ImVec2 top = ImGui::GetCursorScreenPos();
	const float width = std::max(1.0f, ImGui::GetContentRegionAvail().x), trackHeight = font * 2.5f;
	ImGui::InvisibleButton("Captured frames", {width, trackHeight},
						   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
	auto* draw = ImGui::GetWindowDrawList();
	const float left = top.x + font, right = top.x + width - font, lane = top.y + font * .45f;
	const auto muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	draw->AddLine({left, lane}, {right, lane}, ImGui::GetColorU32(ImGuiCol_Border), 2);
	std::uint64_t first = frames.empty() ? 0 : frames.front().frame, last = frames.empty() ? 0 : frames.back().frame;
	for (const auto& event : m_events)
		if (event.run == m_run && event.frame)
		{
			first = first ? std::min(first, event.frame) : event.frame;
			last = std::max(last, event.frame);
		}
	if (!first)
	{
		draw->AddText({left, top.y + font}, muted,
					  "Pause, then choose Next frame to capture a snapshot. Gaps are not retained.");
		ImGui::End();
		return inspect;
	}
	if (m_fit)
	{
		m_start = -1;
		m_span = std::max(2.0, double(last - first) + 2);
	}
	else
		m_start += Offset(m_origin, first);
	m_origin = first;
	const auto& io = ImGui::GetIO();
	if (hovered && io.MouseWheel != 0)
	{
		const double ratio = std::clamp(double(io.MousePos.x - left) / (right - left), 0.0, 1.0);
		const double point = m_start + ratio * m_span;
		m_span = std::clamp(m_span * std::pow(1.5, -io.MouseWheel), 1.0, std::max(2.0, double(last - first) + 2));
		m_start = point - ratio * m_span;
		m_fit = false;
	}
	if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
	{
		m_start -= io.MouseDelta.x * m_span / (right - left);
		m_fit = false;
	}
	const auto x = [&](std::uint64_t frame)
	{
		return left + float((Offset(frame, m_origin) - m_start) / m_span) * (right - left);
	};
	const RuntimeFrame* nearest = nullptr;
	float nearestDistance = std::numeric_limits<float>::max(), previousLabel = left - font;
	const auto accent =
		ImGui::ColorConvertFloat4ToU32(state.manual ? ImVec4{.96f, .69f, .32f, 1} : ImVec4{.48f, .72f, .95f, 1});
	for (const auto& frame : frames)
	{
		const float px = x(frame.frame);
		if (px < left || px > right)
			continue;
		const float distance = std::abs(io.MousePos.x - px);
		if (distance < nearestDistance)
		{
			nearestDistance = distance;
			nearest = &frame;
		}
		const auto color = frame.selected ? accent : frame.imageReady ? IM_COL32(85, 180, 193, 255) : muted;
		if (frame.imageReady)
			draw->AddCircleFilled({px, lane}, font * .27f, color);
		else
			draw->AddCircle({px, lane}, font * .27f, color, 0, 2);
		if (frame.selected)
			draw->AddLine({px, top.y}, {px, top.y + trackHeight}, accent, 2);
		const auto label = std::to_string(frame.frame);
		const auto textWidth = ImGui::CalcTextSize(label.c_str()).x;
		const float labelX = std::clamp(px - textWidth / 2, left, std::max(left, right - textWidth));
		if (labelX > previousLabel + font * .3f)
		{
			draw->AddText({labelX, top.y + font * 1.45f}, color, label.c_str());
			previousLabel = labelX + textWidth;
		}
	}
	if (active && ImGui::IsMouseDown(ImGuiMouseButton_Left) && nearest && !nearest->selected &&
		(io.MousePos.y < top.y + font * .75f || io.MousePos.y > top.y + font * 1.3f))
		choose(*nearest);
	if (hovered && nearest && nearestDistance < font * .6f && io.MousePos.y < top.y + font)
	{
		ImGui::BeginTooltip();
		ImGui::Text("Frame %llu / capture %llu", static_cast<unsigned long long>(nearest->frame),
					static_cast<unsigned long long>(nearest->capture));
		ImGui::TextUnformatted(nearest->imageReady		  ? "Image retained"
							   : nearest->remoteAvailable ? "Image can be requested"
														  : "Image not retained");
		ImGui::TextUnformatted(nearest->stateReady		  ? "Frame state retained"
							   : nearest->remoteAvailable ? "Frame state can be requested"
														  : "Frame state not retained");
		ImGui::TextUnformatted("Click or drag to inspect. Scroll to zoom; middle-drag to pan.");
		ImGui::EndTooltip();
	}
	if (state.sessionAvailable && current == m_run && state.latestFrame)
	{
		const auto px = std::clamp(x(state.latestFrame), left, right);
		const auto color = state.manual ? accent : IM_COL32(85, 205, 165, 255);
		draw->AddTriangleFilled({px - font * .25f, top.y}, {px + font * .25f, top.y}, {px, lane}, color);
		if (hovered && std::abs(io.MousePos.x - px) < font * .35f && io.MousePos.y < lane)
			ImGui::SetTooltip(
				"Application last submitted frame %llu%s", static_cast<unsigned long long>(state.latestFrame),
				x(state.latestFrame) > right || x(state.latestFrame) < left ? " (outside this history view)" : "");
	}
	std::string eventTooltip;
	for (const auto& event : m_events)
	{
		if (event.run != m_run || !event.frame)
			continue;
		const auto px = x(event.frame);
		if (px < left || px > right)
			continue;
		const float py = top.y + font;
		draw->AddCircleFilled({px, py}, font * .18f,
							  event.failed ? IM_COL32(245, 100, 100, 255) : IM_COL32(180, 145, 230, 255));
		if (hovered && std::abs(io.MousePos.x - px) < font * .5f && std::abs(io.MousePos.y - py) < font * .3f)
		{
			if (eventTooltip.size() < 240)
			{
				auto summary = event.text.substr(0, 120);
				std::replace(summary.begin(), summary.end(), '\n', ' ');
				eventTooltip += summary + "\n";
			}
		}
	}
	if (!eventTooltip.empty())
	{
		ImGui::BeginTooltip();
		ImGui::PushTextWrapPos(ImGui::GetCursorPosX() + font * 28);
		ImGui::TextWrapped("Rig observations near this frame (not exact timestamps):\n%s\nClick for full messages.",
						   eventTooltip.c_str());
		ImGui::PopTextWrapPos();
		ImGui::EndTooltip();
		if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
			ImGui::OpenPopup("Timeline rig events");
	}
	ImGui::End();
	return inspect;
}
} // namespace simxr::app
