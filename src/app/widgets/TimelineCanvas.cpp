#include "app/widgets/TimelineCanvas.h"
#include "Pch.h"
#include "app/render/PreviewHistory.h"
#include "app/widgets/Icons.h"
#include "app/widgets/Theme.h"
#include <cmath>
#include <cstdio>
#include <imgui.h>

namespace simxr::app
{
void TimelineCanvas::Zoom(double first, double last) noexcept
{
	m_span = m_zoomSpan = std::max(.05, last - first) * 1.2;
	m_start = (first + last - m_span) / 2;
	m_fit = false;
}
void TimelineCanvas::Begin(float headerWidth, float liveWidth, bool laneNames)
{
	const auto top = ImGui::GetCursorScreenPos();
	const auto available = ImGui::GetContentRegionAvail();
	const float font = ImGui::GetFontSize(), scale = font / 16;
	const float width = std::max(1.f, available.x), height = std::max(1.f, available.y);
	// Lane count follows the window, not this region (guide §3.2).
	const float effectiveWidth = ImGui::GetMainViewport()->Size.x / scale;
	m_compact = height < ImGui::GetFrameHeight() * 2.2f;
	m_lanes = m_compact ? 1 : effectiveWidth < 800 ? 2 : LaneCount;
	const float ruler = m_compact ? 0 : ImGui::GetFrameHeight() + 12 * scale;
	m_live = std::min(liveWidth, width * .2f);
	m_header = std::min(headerWidth, std::max(width * .5f, width - m_live - 80 * scale));
	m_top = top.y;
	m_height = height;
	m_bounds = {top.x + m_header, top.y, std::max(1.f, width - m_header - m_live), height};
	m_transport = {top.x, top.y, m_header, m_compact ? height : ruler};
	m_lane = (height - ruler) / m_lanes;
	m_laneY.fill(-1);
	for (int lane = 0; lane < m_lanes; ++lane)
		m_laneY[lane] = top.y + ruler + lane * m_lane;
	auto* draw = ImGui::GetWindowDrawList();
	if (!m_compact)
		draw->AddRectFilled(top, {top.x + width, top.y + ruler}, theme::Bg);
	else
	{
		draw->AddRectFilled(top, {top.x + m_header, top.y + height}, theme::Bg);
		draw->AddRectFilled({m_bounds.x + m_bounds.width, top.y}, {top.x + width, top.y + height}, theme::Bg);
	}
	static constexpr const char* labels[]{"Frame time", "Images", "Input"};
	static constexpr const char* tips[]{"CPU frame time, or app frame interval when CPU timing is unavailable.",
										"Saved images. Pin snapshots to keep them. Gaps have no saved image.",
										"Hand input: left above, right below. Green marks input read by the app."};
	// Alternate lane shading and one quiet line under the ruler keep the rows apart.
	if (!m_compact)
		draw->AddLine({top.x, top.y + ruler}, {top.x + width, top.y + ruler}, theme::Line);
	for (int lane = 0; lane < m_lanes; ++lane)
	{
		const float y = m_laneY[lane];
		if (lane % 2)
			draw->AddRectFilled({top.x, y}, {top.x + width - m_live, y + m_lane}, theme::Fade(theme::Bg, .45f));
		if (m_compact || m_lane < font || !laneNames)
			continue;
		if (ImGui::IsWindowHovered() && ImGui::IsMouseHoveringRect({top.x, y}, {top.x + m_header, y + m_lane}))
		{
			if (lane == Input && !m_inputDetail.empty())
				ImGui::SetTooltip("%s\n%s", tips[lane], m_inputDetail.c_str());
			else
				ImGui::SetTooltip("%s", tips[lane]);
		}
		draw->AddText({top.x + 8 * scale, y + (m_lane - font) / 2}, theme::TextDim, labels[lane]);
	}
	if (!m_compact)
	{
		draw->AddLine({m_bounds.x, top.y + ruler}, {m_bounds.x, top.y + height}, theme::Line);
		draw->AddLine({m_bounds.x + m_bounds.width, top.y + ruler},
					  {m_bounds.x + m_bounds.width, top.y + height}, theme::Line);
	}
}
TimelineGesture TimelineCanvas::Draw(const HistoryStore& history, PreviewHistory& previews, ImVec4 crop, bool following,
									 std::optional<double> cursor, std::optional<std::pair<double, double>> selection)
{
	TimelineGesture result;
	const int frameNumber = ImGui::GetFrameCount();
	const auto& io = ImGui::GetIO();
	const bool clicked = ImGui::IsMouseClicked(ImGuiMouseButton_Left);
	const bool released = ImGui::IsMouseReleased(ImGuiMouseButton_Left);
	if (m_drawnFrame != frameNumber - 1 || io.AppFocusLost || ImGui::IsKeyPressed(ImGuiKey_Escape) ||
		(!ImGui::IsMouseDown(ImGuiMouseButton_Left) && !released))
	{
		m_framePress.reset();
		m_clipPress.reset();
	}
	if (clicked)
	{
		m_framePress.reset();
		m_clipPress.reset();
	}
	if (m_clipPress && m_clipPress->drawnFrame != frameNumber - 1)
		m_clipPress.reset();
	m_drawnFrame = frameNumber;
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
	m_hovered = hovered;
	if (!active && !released)
	{
		m_framePress.reset();
		m_clipPress.reset();
	}
	const float framesBottom = LaneY(Frames) + m_lane - 1;
	if (!history.Size())
	{
		m_framePress.reset();
		m_clipPress.reset();
		// An empty run still shows where frames will appear.
		draw->AddLine({left, framesBottom}, {right, framesBottom}, muted);
		// The hint sits in the lane below the frame line, or is left out when there is none.
		const char* hint = "Frames, images and input appear here while an app runs";
		const auto size = ImGui::CalcTextSize(hint);
		if (m_lanes > Images && size.x < pixels - 16 * scale)
			draw->AddText({left + (pixels - size.x) / 2, LaneY(Images) + (m_lane - size.y) / 2}, muted, hint);
		ImGui::PopID();
		return result;
	}
	if (m_framePress && !history.Find(m_framePress->frame, m_framePress->epoch))
		m_framePress.reset();
	const double first = history.At(0).seconds, latest = history.At(history.Size() - 1).seconds;
	if (m_fit)
	{
		m_start = first;
		m_span = m_zoomSpan = std::max(.1, latest - first + .01);
		m_fit = false;
	}
	else if (following && latest - first < m_zoomSpan)
	{
		m_start = first;
		m_span = std::max(.5, latest - first + .001);
	}
	else if (following)
	{
		m_span = m_zoomSpan;
		m_start = latest - m_span + .001;
	}
	const auto timeAt = [&](float px)
	{
		return m_start + std::clamp(double(px - left) / pixels, 0., 1.) * m_span;
	};
	if (hovered && io.MouseWheel)
	{
		// While following, zoom keeps the live edge at the right; otherwise zoom around the mouse.
		const auto point = following ? latest : timeAt(io.MousePos.x), ratio = (point - m_start) / m_span;
		m_span = m_zoomSpan = std::clamp(m_span * std::pow(1.5, -io.MouseWheel), .01, std::max(10., latest - first));
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
	if (hovered && active && clicked)
	{
		const double time = timeAt(io.MousePos.x);
		if (const auto index = history.Nearest(time))
		{
			const auto& frame = history.At(*index);
			m_framePress = FramePress{frame.identity, frame.epoch,		  time,
									  io.MousePos,	  FrameTarget::Frame, m_compact || io.MousePos.y < top + ruler};
		}
	}
	std::optional<std::size_t> snapshotHit, markerHit;
	float snapshotDistance = 6 * scale, markerDistance = 6 * scale;
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
		draw->AddRectFilled({X(selection->first), top},
							{std::max(X(selection->first) + 1, X(selection->second)), top + height},
							theme::Fade(theme::Accent, .24f));
	DrawThumbnails(history, previews, crop);
	float lastRunLabel = left - 100 * scale, lastSnapshot = left - 12 * scale;
	// Run names go on the ruler after its numbers, which skip the space they need.
	m_runLabels.clear();
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
		const auto color = missed || work > budget ? theme::Record : work >= budget * .9f ? theme::Warn : theme::Accent;
		const auto bar = std::max(1.f, std::min(m_lane - 2, (m_lane - 2) * work / (2 * budget)));
		draw->AddLine({x, framesBottom}, {x, framesBottom - bar}, color);
		if (m_tint && missed)
			draw->AddLine({x, top}, {x, top + height}, theme::Fade(theme::Record, .22f));
		if (m_lanes > Images)
		{
			const float y = LaneY(Images);
			if (bucket.flags & HistoryPreview)
				draw->AddLine({x, y + m_lane - 2}, {x, y + m_lane - 1}, theme::Accent);
			if ((bucket.flags & HistorySnapshot) && x - lastSnapshot >= 10 * scale)
			{
				// ★ pinned snapshots are filled; unpinned ones may be evicted.
				const float cy = y + m_lane / 2, r = std::min(4 * scale, m_lane / 3);
				if (bucket.flags & HistoryPinned)
					draw->AddQuadFilled({x, cy - r}, {x + r, cy}, {x, cy + r}, {x - r, cy}, theme::Warn);
				else
					draw->AddQuad({x, cy - r}, {x + r, cy}, {x, cy + r}, {x - r, cy}, theme::Warn, 1.5f * scale);
				lastSnapshot = x;
				if (near(x, cy, 6 * scale) && std::abs(io.MousePos.x - x) < snapshotDistance)
				{
					snapshotHit = findFlag(begin, end, HistorySnapshot);
					snapshotDistance = std::abs(io.MousePos.x - x);
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
		if (bucket.flags & HistoryRunStart)
		{
			draw->AddLine({x, top}, {x, top + height}, muted);
			if (!m_compact && x - lastRunLabel > 70 * scale)
			{
				m_runLabels.emplace_back(
					x, "Run " + std::to_string(history.At(findFlag(begin, end, HistoryRunStart)).identity.run));
				lastRunLabel = x;
			}
		}
		if (bucket.flags & HistoryGap)
			draw->AddCircle({x, markerY}, 2.5f * scale, muted);
		if (bucket.flags & HistoryMarker)
		{
			draw->AddTriangleFilled({x, markerY + 3 * scale}, {x - 4 * scale, markerY - 4 * scale},
									{x + 4 * scale, markerY - 4 * scale}, theme::Warn);
			if (near(x, markerY, 6 * scale) && std::abs(io.MousePos.x - x) < markerDistance)
			{
				markerHit = findFlag(begin, end, HistoryMarker);
				markerDistance = std::abs(io.MousePos.x - x);
			}
		}
	}
	result.hovered = snapshotHit ? snapshotHit : markerHit;
	if (m_framePress && clicked)
	{
		if (const auto hit = snapshotHit ? snapshotHit : markerHit)
		{
			const auto& frame = history.At(*hit);
			m_framePress->frame = frame.identity;
			m_framePress->epoch = frame.epoch;
			m_framePress->target = snapshotHit ? FrameTarget::Snapshot : FrameTarget::Marker;
		}
		result.stopFollowing = m_framePress->target != FrameTarget::Snapshot;
	}
	if (m_framePress && released)
	{
		const auto press = *std::exchange(m_framePress, {});
		const auto start = history.Find(press.frame, press.epoch);
		const bool dragged = std::abs(io.MousePos.x - press.position.x) > 5 * scale;
		if (start && press.range && dragged)
		{
			result.first = start;
			result.last = history.Nearest(timeAt(io.MousePos.x));
			if (result.last && *result.first > *result.last)
				std::swap(result.first, result.last);
			result.stopFollowing = true;
		}
		else if (start && press.target == FrameTarget::Snapshot)
		{
			if (snapshotHit == start &&
				io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < io.MouseDragThreshold * io.MouseDragThreshold)
				result.pin = start;
		}
		else if (start && (press.target == FrameTarget::Frame || markerHit == start))
		{
			result.first = start;
			result.stopFollowing = true;
		}
	}
	const auto clearOfRuns = [&](float x, const char* text)
	{
		const float end = x + ImGui::CalcTextSize(text).x;
		for (const auto& [runX, label] : m_runLabels)
			if (x < runX + ImGui::CalcTextSize(label.c_str()).x + 12 * scale && end > runX - 4 * scale)
				return false;
		return true;
	};
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
			if (!clearOfRuns(x + 2, text))
				continue;
			draw->AddText({x + 2, top + (ruler - font) / 2}, muted, text);
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
			if (clearOfRuns(x + 3, text))
				draw->AddText({x + 3, top + (ruler - font) / 2}, muted, text);
		}
	}
	for (const auto& [x, label] : m_runLabels)
		draw->AddText({x + 4 * scale, top + (ruler - font) / 2}, muted, label.c_str());
	if (cursor)
		draw->AddLine({X(*cursor), top}, {X(*cursor), top + height}, theme::Past, 2 * scale);
	if (m_framePress && m_framePress->range)
	{
		const float x = X(m_framePress->seconds);
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
			ImGui::SetTooltip("Frame %llu / run %.3f s\nApplication interval %.2f ms%s%s",
							  static_cast<unsigned long long>(frame.identity.frame), frame.runSeconds,
							  frame.frameMilliseconds,
							  frame.flags & HistorySnapshot ? frame.flags & HistoryPinned ? " / pinned (click to unpin)"
																						  : " / snapshot (click to pin)"
															: "",
							  frame.flags & HistoryPreview ? " / kept preview" : "");
		}
	}
	ImGui::PopID();
	return result;
}

void TimelineCanvas::DrawThumbnails(const HistoryStore& history, PreviewHistory& previews, ImVec4 crop)
{
	previews.BeginThumbnails();
	if (m_lanes <= Images || !history.Size())
		return;
	const float scale = ImGui::GetFontSize() / 16;
	const float height = m_lane - 4 * scale;
	if (height < 4 * scale)
		return;
	const float aspect = previews.Aspect() * (crop.w - crop.y) / (crop.z - crop.x);
	const float width = std::clamp(height / aspect, 16 * scale, 96 * scale);
	auto* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({m_bounds.x, LaneY(Images)}, {m_bounds.x + m_bounds.width, LaneY(Images) + m_lane}, true);
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
		{
			if ((frame.flags & HistoryPreview) && std::abs(frame.seconds - time) <= .2)
				previews.RequestThumbnail(frame.identity, frame.epoch);
			continue;
		}
		previews.RequestThumbnail(shot->frame, shot->epoch);
		const float shownHeight = std::min(height, width * aspect), shownWidth = shownHeight / aspect;
		const float y = LaneY(Images) + (m_lane - shownHeight) / 2;
		previews.DrawThumbnail(*shot, {x, y}, {x + shownWidth, y + shownHeight}, crop);
		// A light veil and hairline keep bright app images from outshouting the frame bars.
		draw->AddRectFilled({x, y}, {x + shownWidth, y + shownHeight}, theme::Fade(theme::Bg, .3f));
		draw->AddRect({x, y}, {x + shownWidth, y + shownHeight}, theme::Line);
	}
	draw->PopClipRect();
}
void TimelineCanvas::DrawEvent(double time, double observed, const char* text, bool failed, bool dim)
{
	const float scale = ImGui::GetFontSize() / 16, x = X(time);
	if (x < m_bounds.x || x > m_bounds.x + m_bounds.width)
		return;
	// Events sit on the ruler's lower edge, beside markers.
	const float y = m_compact ? m_top + 5 * scale : m_top + m_transport.height - 5 * scale;
	const auto color = failed ? theme::Record : dim ? ImGui::GetColorU32(ImGuiCol_TextDisabled) : theme::Head;
	ImGui::GetWindowDrawList()->AddCircle({x, y}, 3.5f * scale, color, 0, 1.5f * scale);
	const auto mouse = ImGui::GetIO().MousePos;
	if (std::abs(mouse.x - x) < 6 * scale && std::abs(mouse.y - y) < 6 * scale && ImGui::IsWindowHovered())
		ImGui::SetTooltip("%s%s\nObserved +%.2fs (nearest observed frame)", text,
						  dim ? " (another app or session)" : "", observed);
}
ClipHit TimelineCanvas::DrawClip(const char* id, double first, double last, const char* label, bool replay,
								 bool playable, bool stoppable)
{
	if (last < m_start || first > m_start + m_span)
		return ClipHit::None;
	const float scale = ImGui::GetFontSize() / 16;
	const float top = m_laneY[0], bottom = m_top + m_height;
	const float x = std::max(m_bounds.x, X(first)),
				end = std::min(m_bounds.x + m_bounds.width, std::max(X(first) + 2, X(last)));
	const auto color = replay ? theme::Past : theme::Record;
	auto* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({m_bounds.x, m_top}, {m_bounds.x + m_bounds.width, bottom}, true);
	draw->AddRectFilled({x, top}, {end, bottom}, theme::Fade(color, .2f));
	draw->AddLine({x, top + scale}, {end, top + scale}, color, 2 * scale);
	if (X(first) >= m_bounds.x)
		draw->AddLine({x, top}, {x, bottom}, theme::Fade(color, .7f), scale);
	if (X(last) <= m_bounds.x + m_bounds.width)
		draw->AddLine({end, top}, {end, bottom}, theme::Fade(color, .7f), scale);
	// The name tab at the top-left, with a play button in front for a saved take
	// or a stop button for a running replay.
	const float font = ImGui::GetFontSize(), tabHeight = std::min(font + 4 * scale, m_lane);
	const bool action = playable || stoppable;
	const float play = action ? tabHeight : 0;
	const float tabWidth = play + ImGui::CalcTextSize(label).x + 10 * scale;
	const ImVec2 tabMin{x, top}, tabMax{x + tabWidth, top + tabHeight};
	const auto& io = ImGui::GetIO();
	const auto mouse = io.MousePos;
	const bool hovered = m_hovered && ImGui::IsMouseHoveringRect(tabMin, tabMax);
	ImGui::PushID(this);
	const auto clipId = ImGui::GetID(id);
	ImGui::PopID();
	if (m_clipPress && m_clipPress->id == clipId)
		m_clipPress->drawnFrame = ImGui::GetFrameCount();
	draw->AddRectFilled(tabMin, tabMax, hovered ? color : theme::Mix(theme::Surface, color, .75f),
						theme::Radius * scale, ImDrawFlags_RoundCornersBottomRight);
	const bool overPlay = hovered && action && mouse.x < tabMin.x + play;
	if (action)
	{
		if (overPlay)
			draw->AddRectFilled(tabMin, {tabMin.x + play, tabMax.y}, theme::Accent);
		DrawIcon(draw, stoppable ? Icon::Stop : Icon::Play, {tabMin.x + play / 2, top + tabHeight / 2}, tabHeight * .7f,
				 theme::Text);
	}
	draw->AddText({tabMin.x + play + 5 * scale, top + (tabHeight - font) / 2}, theme::Text, label);
	draw->PopClipRect();
	if (!hovered)
		return ClipHit::None;
	const auto actionHit = overPlay ? (stoppable ? ClipHit::Stop : ClipHit::Play)
						   : ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left) ? ClipHit::Opened
																				: ClipHit::Clicked;
	if (ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		m_clipPress = ClipPress{clipId, actionHit, ImGui::GetFrameCount()};
		m_framePress.reset();
	}
	ClipHit result = ClipHit::None;
	if (ImGui::IsMouseReleased(ImGuiMouseButton_Left) && m_clipPress && m_clipPress->id == clipId &&
		io.MouseDragMaxDistanceSqr[ImGuiMouseButton_Left] < io.MouseDragThreshold * io.MouseDragThreshold)
	{
		const auto pressed = m_clipPress->action;
		if (pressed == actionHit || (!overPlay && pressed == ClipHit::Opened))
		{
			result = pressed;
			m_clipPress.reset();
		}
	}
	if (overPlay && stoppable)
		ImGui::SetTooltip("Stop %s and release its inputs", label);
	else if (overPlay)
		ImGui::SetTooltip("Play %s. This restores inputs, not game state.", label);
	else
		ImGui::SetTooltip("%s\nClick selects its frames. Double-click opens it", label);
	return result;
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
		draw->AddText({x + 4 * scale, y + (m_lane - size.y) / 2}, theme::Paused, label);
	draw->PopClipRect();
	const auto mouse = ImGui::GetIO().MousePos;
	if (mouse.x >= x && mouse.x < end && mouse.y >= y && mouse.y < y + m_lane && ImGui::IsWindowHovered())
		ImGui::SetTooltip("App paused for %.1f s\nIt only advanced when stepped. Continue resumes it", last - first);
}
void TimelineCanvas::DrawGhost(const HistoryStore& history, std::size_t sourceFirst, std::size_t sourceLast,
							   std::size_t targetFirst, std::size_t targetLast)
{
	if (m_lanes <= Input || sourceFirst > sourceLast || targetFirst > targetLast || sourceLast >= history.Size() ||
		targetLast >= history.Size())
		return;
	sourceLast = std::min(sourceLast, history.SegmentAt(sourceFirst).second - 1);
	targetLast = std::min(targetLast, history.SegmentAt(targetFirst).second - 1);
	const auto sourceFrame = history.At(sourceFirst).identity.frame;
	const auto targetFrame = history.At(targetFirst).identity.frame;
	const auto offset = std::min(history.At(sourceLast).identity.frame - sourceFrame,
								 history.At(targetLast).identity.frame - targetFrame);
	const auto frameBound = [&](std::size_t first, std::size_t end, std::uint64_t frame, bool after)
	{
		while (first < end)
		{
			const auto middle = first + (end - first) / 2;
			const auto value = history.At(middle).identity.frame;
			if (value < frame || (after && value == frame))
				first = middle + 1;
			else
				end = middle;
		}
		return first;
	};
	const auto targetEnd = frameBound(targetFirst, targetLast + 1, targetFrame + offset, true);
	const float scale = ImGui::GetFontSize() / 16, y = LaneY(Input), middle = y + m_lane / 2, reach = m_lane / 2 - 2;
	auto* draw = ImGui::GetWindowDrawList();
	draw->PushClipRect({m_bounds.x, y}, {m_bounds.x + m_bounds.width, y + m_lane}, true);
	const auto ink = theme::Fade(theme::Text, .8f);
	for (int column = 0; column < int(m_bounds.width); ++column)
	{
		const auto begin = std::max(targetFirst, history.LowerBound(m_start + column * m_span / m_bounds.width));
		const auto end = std::min(targetEnd, history.LowerBound(m_start + (column + 1) * m_span / m_bounds.width));
		if (begin >= end)
			continue;
		const auto firstFrame = history.At(begin).identity.frame, lastFrame = history.At(end - 1).identity.frame;
		const auto target = history.Query(begin, end);
		// A mixed pixel cannot faithfully compare missing or released input.
		if (lastFrame - firstFrame != end - begin - 1 || target.driven != target.count)
			continue;
		const auto expectedFirst = sourceFrame + (firstFrame - targetFrame);
		const auto expectedLast = sourceFrame + (lastFrame - targetFrame);
		const auto first = frameBound(sourceFirst, sourceLast + 1, expectedFirst, false);
		const auto last = frameBound(first, sourceLast + 1, expectedLast, false);
		if (first > sourceLast || last > sourceLast || history.At(first).identity.frame != expectedFirst ||
			history.At(last).identity.frame != expectedLast || last - first != end - begin - 1)
			continue;
		const auto bucket = history.Query(first, last + 1);
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
