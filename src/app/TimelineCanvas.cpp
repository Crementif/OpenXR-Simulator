#include "app/TimelineCanvas.h"
#include "app/Theme.h"
#include "app/PreviewHistory.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>

namespace simxr::app
{
namespace
{
constexpr ImU32 Accent = theme::Accent, MarkerColor = theme::Warn, Alert = theme::Record, Paused = theme::Paused;
}
void TimelineCanvas::Zoom(double first, double last) noexcept
{
	m_span = std::max(.05, last - first) * 1.2;
	m_start = (first + last - m_span) / 2;
	m_fit = false;
}
void TimelineCanvas::Begin(float headerWidth, float liveWidth)
{
	const auto top = ImGui::GetCursorScreenPos();
	const auto available = ImGui::GetContentRegionAvail();
	const float font = ImGui::GetFontSize(), scale = font / 16;
	const float width = std::max(1.f, available.x), height = std::max(1.f, available.y);
	// Lane count follows the window, not this region (guide §3.2).
	const float effectiveWidth = ImGui::GetMainViewport()->Size.x / scale;
	m_compact = height < ImGui::GetFrameHeight() * 2.2f;
	m_lanes = m_compact ? 1 : effectiveWidth < 800 ? 2 : LaneCount;
	const float ruler = m_compact ? 0 : ImGui::GetFrameHeight() + 2 * scale;
	m_header = std::min(headerWidth, width * .5f);
	m_live = std::min(liveWidth, width * .2f);
	m_top = top.y;
	m_height = height;
	m_bounds = {top.x + m_header, top.y, std::max(1.f, width - m_header - m_live), height};
	m_transport = {top.x, top.y, m_header, m_compact ? height : ruler};
	m_lane = (height - ruler) / m_lanes;
	m_laneY.fill(-1);
	for (int lane = 0; lane < m_lanes; ++lane)
		m_laneY[lane] = top.y + ruler + lane * m_lane;
	auto* draw = ImGui::GetWindowDrawList();
	const auto muted = ImGui::GetColorU32(ImGuiCol_TextDisabled), line = ImGui::GetColorU32(ImGuiCol_Border);
	static constexpr const char* labels[]{"Frame time", "Images", "Input", "Recording", "Events"};
	static constexpr const char* tips[]{
		"CPU frame span against the display period; falls back to display interval when timing is unavailable",
		"Kept preview coverage; snapshots can be pinned. Gaps have no saved image.",
		"Applied hand inputs; green marks actions read by the app",
		"Red: recorded inputs. Blue: replay or script. Hover a saved take to play it.",
		"App and script events; hollow marks use the nearest observed frame"};
	for (int lane = 0; lane < m_lanes; ++lane)
	{
		const float y = m_laneY[lane];
		if (!m_compact && ImGui::IsWindowHovered() &&
			ImGui::IsMouseHoveringRect({top.x, y}, {top.x + m_header, y + m_lane}))
			ImGui::SetTooltip("%s", tips[lane]);
		draw->AddLine({top.x, y}, {top.x + width, y}, line);
		if (!m_compact && m_lane >= font)
			draw->AddText({top.x + 6 * scale, y + (m_lane - font) / 2}, muted, labels[lane]);
	}
	draw->AddLine({m_bounds.x, top.y}, {m_bounds.x, top.y + height}, line);
	draw->AddLine({m_bounds.x + m_bounds.width, top.y}, {m_bounds.x + m_bounds.width, top.y + height}, line);
}
TimelineGesture TimelineCanvas::Draw(const HistoryStore& history, PreviewHistory& previews, ImVec4 crop, bool following, std::optional<double> cursor,
									 std::optional<std::pair<double, double>> selection)
{
	TimelineGesture result;
	const float font = ImGui::GetFontSize(), scale = font / 16;
	const float left = m_bounds.x, right = left + m_bounds.width, pixels = m_bounds.width;
	const float top = m_top, height = m_height, ruler = m_transport.height;
	auto* draw = ImGui::GetWindowDrawList();
	const auto muted = ImGui::GetColorU32(ImGuiCol_TextDisabled);
	ImGui::PushID(this);
	ImGui::SetCursorScreenPos({left, top});
	ImGui::InvisibleButton("Timeline canvas", {pixels, height},
						   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
	const float framesBottom = LaneY(Frames) + m_lane - 1;
	if (!history.Size())
	{
		// An empty run still shows where frames will appear.
		draw->AddLine({left, framesBottom}, {right, framesBottom}, muted);
		ImGui::PopID();
		return result;
	}
	const double first = history.At(0).seconds, latest = history.At(history.Size() - 1).seconds;
	if (m_fit)
	{
		m_start = first;
		m_span = std::max(.1, latest - first + .01);
		m_fit = false;
	}
	else if (following)
		m_start = latest - m_span + .001;
	const auto& io = ImGui::GetIO();
	const auto timeAt = [&](float px)
	{
		return m_start + std::clamp(double(px - left) / pixels, 0., 1.) * m_span;
	};
	if (hovered && io.MouseWheel)
	{
		// While following, zoom keeps the live edge at the right; otherwise zoom around the mouse.
		const auto point = following ? latest : timeAt(io.MousePos.x), ratio = (point - m_start) / m_span;
		m_span = std::clamp(m_span * std::pow(1.5, -io.MouseWheel), .01, std::max(10., latest - first));
		m_start = following ? latest - m_span + .001 : point - ratio * m_span;
	}
	if (active && ImGui::IsMouseDragging(ImGuiMouseButton_Middle))
	{
		m_start -= io.MouseDelta.x * m_span / pixels;
		result.stopFollowing = true;
	}
	if (hovered && !io.WantTextInput)
	{
		if (ImGui::IsKeyPressed(ImGuiKey_Home))
		{
			m_fit = true;
			result.stopFollowing = true;
		}
		if (ImGui::IsKeyPressed(ImGuiKey_M))
			result.marker = history.Nearest(following ? latest : cursor.value_or(timeAt(io.MousePos.x)));
		if (ImGui::IsKeyPressed(ImGuiKey_End))
			result.goLive = true;
	}
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		m_dragStart = timeAt(io.MousePos.x);
		m_rangeDrag = m_compact || io.MousePos.y < top + ruler;
	}
	if (m_dragStart && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		const auto end = timeAt(io.MousePos.x);
		result.first = history.Nearest(*m_dragStart);
		if (m_rangeDrag && std::abs(end - *m_dragStart) * pixels / m_span > 5 * scale)
			result.last = history.Nearest(end);
		if (result.last && *result.first > *result.last)
			std::swap(result.first, result.last);
		result.stopFollowing = true;
		m_dragStart.reset();
	}
	// Finds one flagged frame within a dense pixel column without scanning its frames.
	const auto findFlag = [&](std::size_t a, std::size_t b, std::uint32_t flag)
	{
		while (a + 1 < b)
		{
			const auto middle = a + (b - a) / 2;
			if (history.Query(a, middle).flags & flag)
				b = middle;
			else
				a = middle;
		}
		return a;
	};
	const auto near = [&](float x, float y, float radius)
	{
		return hovered && std::abs(io.MousePos.x - x) < radius && std::abs(io.MousePos.y - y) < radius;
	};
	draw->PushClipRect({left, top}, {right, top + height}, true);
	if (selection)
		draw->AddRectFilled({X(selection->first), top}, {std::max(X(selection->first) + 1, X(selection->second)),
														 top + height},
							theme::Fade(theme::Accent, .24f));
	DrawThumbnails(history, previews, crop);
	float lastRunLabel = left - 100 * scale, lastSnapshot = left - 12 * scale;
	const float budget = m_budget > 0 ? m_budget : 33.3f / 2;
	// The budget line: frames under it made their display time.
	{
		const float y = framesBottom - (m_lane - 2) / 2;
		for (float dash = left; dash < right; dash += 8 * scale)
			draw->AddLine({dash, y}, {std::min(right, dash + 4 * scale), y}, theme::Fade(theme::TextDim, .5f), scale);
	}
	const float markerY = m_compact ? top + 5 * scale : top + ruler - 5 * scale;
	for (int column = 0; column < int(pixels); ++column)
	{
		const auto begin = history.LowerBound(m_start + column * m_span / pixels);
		const auto end = history.LowerBound(m_start + (column + 1) * m_span / pixels);
		const auto bucket = history.Query(begin, end);
		if (!bucket.count)
			continue;
		const float x = left + column;
		// Bars reach half the lane at the budget; amber above it, red for a missed frame.
		const bool missed = bucket.flags & (HistoryLate | HistoryDropped);
		const float work = bucket.cpuMaximum >= 0 ? bucket.cpuMaximum : bucket.maximum;
		const auto color = missed || work > budget ? Alert : work >= budget * .9f ? theme::Warn : Accent;
		const auto bar = std::max(1.f, std::min(m_lane - 2, (m_lane - 2) * work / (2 * budget)));
		draw->AddLine({x, framesBottom}, {x, framesBottom - bar}, color);
		if (m_tint && missed)
			draw->AddLine({x, top}, {x, top + height}, theme::Fade(Alert, .22f));
		if (m_lanes > Images)
		{
			const float y = LaneY(Images);
			if (bucket.flags & HistoryPreview)
				draw->AddLine({x, y + m_lane - 2}, {x, y + m_lane - 1}, Accent);
			if ((bucket.flags & HistorySnapshot) && x - lastSnapshot >= 10 * scale)
			{
				// ★ pinned snapshots are filled; unpinned ones may be evicted.
				const float cy = y + m_lane / 2, r = std::min(4 * scale, m_lane / 3);
				if (bucket.flags & HistoryPinned)
					draw->AddQuadFilled({x, cy - r}, {x + r, cy}, {x, cy + r}, {x - r, cy}, MarkerColor);
				else
					draw->AddQuad({x, cy - r}, {x + r, cy}, {x, cy + r}, {x - r, cy}, MarkerColor, 1.5f * scale);
				lastSnapshot = x;
				if (near(x, cy, 6 * scale))
				{
					const auto snapshot = findFlag(begin, end, HistorySnapshot);
					result.hovered = snapshot;
					if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && !result.last)
					{
						result.pin = snapshot;
						result.first.reset();
						result.stopFollowing = false;
					}
				}
			}
		}
		if (m_lanes > Input)
		{
			// Left presses rise from the middle, right presses fall from it.
			const float y = LaneY(Input), middle = y + m_lane / 2, reach = m_lane / 2 - 2;
			if (bucket.flags & HistoryDriven)
				draw->AddLine({x, y + 1}, {x, y + m_lane - 1}, theme::Fade(theme::Past, .22f));
			if (bucket.hands[0])
				draw->AddLine({x, middle}, {x, middle - std::max(1.f, reach * bucket.hands[0] / 255)}, theme::Left);
			if (bucket.hands[1])
				draw->AddLine({x, middle}, {x, middle + std::max(1.f, reach * bucket.hands[1] / 255)}, theme::Right);
			if (bucket.flags & HistoryAction)
				draw->AddLine({x, y + m_lane - 3 * scale}, {x, y + m_lane - 1}, theme::Ok);
		}
		if (m_lanes > Recording && (bucket.flags & HistoryRecording))
			draw->AddLine({x, LaneY(Recording) + 2}, {x, LaneY(Recording) + m_lane - 2}, Alert);
		if (bucket.flags & HistoryRunStart)
		{
			draw->AddLine({x, top}, {x, top + height}, muted);
			if (!m_compact && x - lastRunLabel > 70 * scale)
			{
				char label[48];
				std::snprintf(label, sizeof(label), "Run %llu",
							  static_cast<unsigned long long>(history.At(begin).identity.run));
				draw->AddText({x + 3, top + height - font}, muted, label);
				lastRunLabel = x;
			}
		}
		if (m_lanes > Events && (bucket.flags & HistoryGap))
			draw->AddCircle({x, LaneY(Events) + m_lane / 2}, 3 * scale, muted);
		if (bucket.flags & HistoryMarker)
		{
			draw->AddTriangleFilled({x, markerY + 3 * scale}, {x - 4 * scale, markerY - 4 * scale},
									{x + 4 * scale, markerY - 4 * scale}, MarkerColor);
			if (near(x, markerY, 6 * scale))
			{
				result.hovered = findFlag(begin, end, HistoryMarker);
				if (ImGui::IsMouseReleased(ImGuiMouseButton_Left))
				{
					result.first = result.hovered;
					result.last.reset();
				}
			}
		}
	}
	const auto visibleFirst = history.LowerBound(m_start), visibleEnd = history.LowerBound(m_start + m_span);
	if (!m_compact && visibleEnd > visibleFirst && pixels / (visibleEnd - visibleFirst) >= 6 * scale)
	{
		float previousLabel = left - 70 * scale;
		for (auto index = visibleFirst; index < visibleEnd; ++index)
		{
			const auto& frame = history.At(index);
			const float x = X(frame.seconds);
			draw->AddLine({x, top + ruler - 4}, {x, top + ruler}, muted);
			if (x - previousLabel < 64 * scale)
				continue;
			char text[32];
			std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(frame.identity.frame));
			draw->AddText({x + 2, top + 2 * scale}, muted, text);
			previousLabel = x;
		}
	}
	else if (!m_compact)
	{
		// Labels sample runtime-relative time, never the artificial inter-run offset.
		for (float x = left; x < right; x += std::max(80 * scale, pixels / 8))
		{
			const auto index = history.Nearest(timeAt(x));
			if (!index || timeAt(x) < first || timeAt(x) > latest)
				continue;
			char text[32];
			std::snprintf(text, sizeof(text), "%.2fs", history.At(*index).runSeconds);
			draw->AddText({x + 3, top + 2 * scale}, muted, text);
		}
	}
	if (cursor)
		draw->AddLine({X(*cursor), top}, {X(*cursor), top + height}, theme::Past, 2 * scale);
	if (m_dragStart && m_rangeDrag)
	{
		const float x = X(*m_dragStart);
		draw->AddRectFilled({std::min(x, io.MousePos.x), top}, {std::max(x, io.MousePos.x), top + height},
							theme::Fade(theme::Accent, .24f));
	}
	draw->PopClipRect();
	if (hovered)
	{
		if (!result.hovered)
			result.hovered = history.Nearest(timeAt(io.MousePos.x));
		if (result.hovered)
		{
			const auto& frame = history.At(*result.hovered);
			ImGui::SetTooltip("Frame %llu / run %.3f s\nDisplay interval %.2f ms%s%s",
							  static_cast<unsigned long long>(frame.identity.frame), frame.runSeconds,
							  frame.frameMilliseconds,
							  frame.flags & HistorySnapshot
								  ? frame.flags & HistoryPinned ? " / pinned (click to unpin)"
																: " / snapshot (click to pin)"
								  : "",
							  frame.flags & HistoryPreview ? " / kept preview" : "");
		}
	}
	ImGui::PopID();
	return result;
}

void TimelineCanvas::DrawInputLabel(const char* text)
{
	if (m_lanes <= Input || !*text)
		return;
	const ImVec2 start{m_bounds.x + 4, LaneY(Input) + 2};
	const ImVec2 end{std::min(m_bounds.x + m_bounds.width, start.x + ImGui::CalcTextSize(text).x + 8),
					std::min(LaneY(Input) + m_lane, start.y + ImGui::GetFontSize() + 2)};
	auto* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect(start, end, true);
	draw->AddRectFilled(start, end, ImGui::GetColorU32(ImGuiCol_WindowBg));
	draw->AddText(start, theme::TextDim, text);
	draw->PopClipRect();
	if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect(start, end))
		ImGui::SetTooltip("%s", text);
}
void TimelineCanvas::DrawThumbnails(const HistoryStore& history, PreviewHistory& previews, ImVec4 crop)
{
	if (m_lanes <= Images || !history.Size())
		return;
	const float scale = ImGui::GetFontSize() / 16;
	const float height = m_lane - 4 * scale;
	if (height < 4 * scale)
		return;
	const float aspect = previews.Aspect() * (crop.w - crop.y) / (crop.z - crop.x);
	const float width = std::clamp(height / aspect,
								  16 * scale, 96 * scale);
	auto* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({m_bounds.x, LaneY(Images)},
					  {m_bounds.x + m_bounds.width, LaneY(Images) + m_lane}, true);
	for (float x = m_bounds.x; x + width <= m_bounds.x + m_bounds.width; x += width + 3 * scale)
	{
		const auto index = history.Nearest(m_start + (x + width / 2 - m_bounds.x) * m_span / m_bounds.width);
		if (!index)
			continue;
		const auto& frame = history.At(*index);
		const auto shot = previews.Nearest(frame.identity, frame.epoch);
		const auto source = shot ? history.Find(shot->frame, shot->epoch) : std::nullopt;
		// Do not stretch a kept image over a gap or an empty part of the timeline.
		const double time = m_start + (x + width / 2 - m_bounds.x) * m_span / m_bounds.width;
		if (!source || std::abs(history.At(*source).seconds - time) > .2)
			continue;
		const float shownHeight = std::min(height, width * aspect), shownWidth = shownHeight / aspect;
		const float y = LaneY(Images) + (m_lane - shownHeight) / 2;
		previews.DrawThumbnail(*shot, {x, y}, {x + shownWidth, y + shownHeight}, crop);
	}
	draw->PopClipRect();
}
void TimelineCanvas::DrawEvent(double time, double observed, const char* text, bool failed, bool dim)
{
	if (m_lanes <= Events)
		return;
	const float scale = ImGui::GetFontSize() / 16, x = X(time), y = LaneY(Events) + m_lane / 2;
	if (x < m_bounds.x || x > m_bounds.x + m_bounds.width)
		return;
	const auto color = failed ? Alert : dim ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : theme::Head;
	ImGui::GetWindowDrawList()->AddCircle({x, y}, 4 * scale, color, 0, 2);
	const auto mouse = ImGui::GetIO().MousePos;
	if (std::abs(mouse.x - x) < 6 * scale && std::abs(mouse.y - y) < 6 * scale && ImGui::IsWindowHovered())
		ImGui::SetTooltip("%s%s\nObserved +%.2fs (nearest observed frame)", text,
						  dim ? " (another app or session)" : "", observed);
}
ClipHit TimelineCanvas::DrawClip(double first, double last, const char* label, bool replay, bool playable)
{
	if (m_lanes <= Recording || last < m_start || first > m_start + m_span)
		return ClipHit::None;
	const float y = LaneY(Recording);
	const float x = X(std::max(first, m_start)), end = std::max(x + 2, X(std::min(last, m_start + m_span)));
	auto* draw = ImGui::GetWindowDrawList();
	draw->AddRect({x, y + 2}, {end, y + m_lane - 2}, replay ? theme::Past : Alert);
	const auto mouse = ImGui::GetIO().MousePos;
	if (mouse.x < x || mouse.x > end || mouse.y < y || mouse.y >= y + m_lane || !ImGui::IsWindowHovered())
		return ClipHit::None;
	if (playable)
	{
		const float size = m_lane - 4, left = std::max(m_bounds.x, std::min(x + 2, end - size));
		const bool over = mouse.x >= left && mouse.x < left + size;
		draw->AddRectFilled({left, y + 2}, {left + size, y + m_lane - 2}, over ? theme::Accent : theme::SurfaceRaised,
							theme::Radius);
		const float inset = size * .3f;
		draw->AddTriangleFilled({left + inset, y + 2 + inset}, {left + inset, y + m_lane - 2 - inset},
								{left + size - inset, y + m_lane / 2}, theme::Text);
		if (over)
		{
			ImGui::SetTooltip("Play %s on the app; restores inputs, not game state", label);
			return ImGui::IsMouseReleased(ImGuiMouseButton_Left) ? ClipHit::Play : ClipHit::None;
		}
	}
	ImGui::SetTooltip("%s\nClick selects its frames; double-click opens the library", label);
	if (ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
		return ClipHit::Opened;
	return ImGui::IsMouseReleased(ImGuiMouseButton_Left) ? ClipHit::Clicked : ClipHit::None;
}
void TimelineCanvas::DrawPause(double first, double last)
{
	if (last < m_start || first > m_start + m_span)
		return;
	const float scale = ImGui::GetFontSize() / 16, y = LaneY(Frames);
	const float x = std::max(m_bounds.x, X(first)),
				end = std::min(m_bounds.x + m_bounds.width, std::max(X(first) + 3 * scale, X(last)));
	auto* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({x, y}, {end, y + m_lane}, true);
	draw->AddRectFilled({x, y}, {end, y + m_lane}, theme::Mix(theme::Surface, theme::Paused, .22f));
	const char* label = "paused";
	const auto size = ImGui::CalcTextSize(label);
	if (end - x > size.x + 8 * scale && m_lane >= size.y)
		draw->AddText({x + 4 * scale, y + (m_lane - size.y) / 2}, Paused, label);
	draw->PopClipRect();
	const auto mouse = ImGui::GetIO().MousePos;
	if (mouse.x >= x && mouse.x < end && mouse.y >= y && mouse.y < y + m_lane && ImGui::IsWindowHovered())
		ImGui::SetTooltip("App paused for %.1f s\nIt only advanced when stepped; Continue resumes it", last - first);
}
void TimelineCanvas::DrawGhost(const HistoryStore& history, std::size_t source, std::size_t anchor, std::size_t count)
{
	if (m_lanes <= Input || !history.Size())
		return;
	const float scale = ImGui::GetFontSize() / 16, y = LaneY(Input), middle = y + m_lane / 2, reach = m_lane / 2 - 2;
	auto* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({m_bounds.x, y}, {m_bounds.x + m_bounds.width, y + m_lane}, true);
	const auto ink = theme::Fade(theme::Text, .8f);
	for (int column = 0; column < int(m_bounds.width); ++column)
	{
		const auto begin = history.LowerBound(m_start + column * m_span / m_bounds.width);
		const auto end = history.LowerBound(m_start + (column + 1) * m_span / m_bounds.width);
		if (begin >= end || end <= anchor || begin >= anchor + count)
			continue;
		const auto a = std::max(begin, anchor) - anchor, b = std::min(end, anchor + count) - anchor;
		if (source + b > history.Size())
			continue;
		const auto bucket = history.Query(source + a, source + b);
		const float x = m_bounds.x + column;
		// A tick where the recorded value was; it sits on the bar tip when replay matches.
		for (int hand = 0; hand < 2; ++hand)
			if (bucket.hands[hand])
			{
				const float tip = middle + (hand ? reach : -reach) * bucket.hands[hand] / 255;
				draw->AddLine({x - scale, tip}, {x + scale, tip}, ink);
			}
	}
	draw->PopClipRect();
}
} // namespace simxr::app
