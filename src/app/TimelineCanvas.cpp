#include "app/TimelineCanvas.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>

namespace simxr::app
{
namespace
{
constexpr ImU32 Accent = IM_COL32(47, 143, 163, 255), MarkerColor = IM_COL32(229, 181, 58, 255),
				Alert = IM_COL32(229, 72, 77, 255), Paused = IM_COL32(240, 164, 58, 255);
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
	m_lanes = m_compact ? 1 : effectiveWidth < 800 ? 2 : 4;
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
	static constexpr const char* labels[]{"Frames", "Images", "Recording", "Events"};
	for (int lane = 0; lane < m_lanes; ++lane)
	{
		const float y = m_laneY[lane];
		draw->AddLine({top.x, y}, {top.x + width, y}, line);
		if (!m_compact && m_lane >= font)
			draw->AddText({top.x + 6 * scale, y + (m_lane - font) / 2}, muted, labels[lane]);
	}
	draw->AddLine({m_bounds.x, top.y}, {m_bounds.x, top.y + height}, line);
	draw->AddLine({m_bounds.x + m_bounds.width, top.y}, {m_bounds.x + m_bounds.width, top.y + height}, line);
}
TimelineGesture TimelineCanvas::Draw(const HistoryStore& history, bool following, std::optional<double> cursor,
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
							IM_COL32(47, 143, 163, 60));
	float lastRunLabel = left - 100 * scale, lastSnapshot = left - 12 * scale;
	const float markerY = m_compact ? top + 5 * scale : top + ruler - 5 * scale;
	for (int column = 0; column < int(pixels); ++column)
	{
		const auto begin = history.LowerBound(m_start + column * m_span / pixels);
		const auto end = history.LowerBound(m_start + (column + 1) * m_span / pixels);
		const auto bucket = history.Query(begin, end);
		if (!bucket.count)
			continue;
		const float x = left + column;
		const auto color = bucket.flags & (HistoryLate | HistoryDropped) ? Alert : Accent;
		const auto bar = std::max(0.f, std::min(m_lane - 2, m_lane * bucket.maximum / 33.3f));
		draw->AddLine({x, framesBottom}, {x, framesBottom - bar}, color);
		if (m_lanes > Images)
		{
			const float y = LaneY(Images);
			if (bucket.flags & HistoryPreview)
				draw->AddLine({x, y + 2}, {x, y + m_lane - 2}, Accent);
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
		draw->AddLine({X(*cursor), top}, {X(*cursor), top + height}, IM_COL32(150, 200, 255, 255), 2 * scale);
	if (m_dragStart && m_rangeDrag)
	{
		const float x = X(*m_dragStart);
		draw->AddRectFilled({std::min(x, io.MousePos.x), top}, {std::max(x, io.MousePos.x), top + height},
							IM_COL32(47, 143, 163, 60));
	}
	draw->PopClipRect();
	if (hovered)
	{
		if (!result.hovered)
			result.hovered = history.Nearest(timeAt(io.MousePos.x));
		if (result.hovered)
		{
			const auto& frame = history.At(*result.hovered);
			ImGui::SetTooltip("Run %llu / frame %llu\nRun time %.3fs\nDisplay interval %.2f ms%s%s",
							  static_cast<unsigned long long>(frame.identity.run),
							  static_cast<unsigned long long>(frame.identity.frame), frame.runSeconds,
							  frame.frameMilliseconds,
							  frame.flags & HistorySnapshot
								  ? frame.flags & HistoryPinned ? "\nPinned snapshot (click to unpin)"
																: "\nSnapshot (click to pin)"
								  : "",
							  frame.flags & HistoryPreview ? "\nPreview" : "");
		}
	}
	ImGui::PopID();
	return result;
}
void TimelineCanvas::DrawEvent(double time, double observed, const char* text, bool failed, bool dim)
{
	if (m_lanes <= Events)
		return;
	const float scale = ImGui::GetFontSize() / 16, x = X(time), y = LaneY(Events) + m_lane / 2;
	if (x < m_bounds.x || x > m_bounds.x + m_bounds.width)
		return;
	const auto color = failed ? Alert : dim ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : IM_COL32(201, 167, 255, 255);
	ImGui::GetWindowDrawList()->AddCircle({x, y}, 4 * scale, color, 0, 2);
	const auto mouse = ImGui::GetIO().MousePos;
	if (std::abs(mouse.x - x) < 6 * scale && std::abs(mouse.y - y) < 6 * scale && ImGui::IsWindowHovered())
		ImGui::SetTooltip("%s%s\nObserved +%.2fs (nearest observed frame)", text,
						  dim ? "\nAnother app or session" : "", observed);
}
ClipHit TimelineCanvas::DrawClip(double first, double last, const char* label, bool replay)
{
	if (m_lanes <= Recording || last < m_start || first > m_start + m_span)
		return ClipHit::None;
	const float y = LaneY(Recording);
	const float x = X(std::max(first, m_start)), end = std::max(x + 2, X(std::min(last, m_start + m_span)));
	ImGui::GetWindowDrawList()->AddRect({x, y + 2}, {end, y + m_lane - 2},
										replay ? IM_COL32(80, 150, 230, 255) : Alert);
	const auto mouse = ImGui::GetIO().MousePos;
	if (mouse.x < x || mouse.x > end || mouse.y < y || mouse.y >= y + m_lane || !ImGui::IsWindowHovered())
		return ClipHit::None;
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
	draw->AddRectFilled({x, y}, {end, y + m_lane}, IM_COL32(240, 164, 58, 40));
	for (float hatch = x - m_lane; hatch < end; hatch += 6 * scale)
		draw->AddLine({hatch, y + m_lane}, {hatch + m_lane, y}, Paused, scale);
	draw->PopClipRect();
}
} // namespace simxr::app
