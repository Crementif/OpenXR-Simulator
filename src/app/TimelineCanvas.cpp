#include "app/TimelineCanvas.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui.h>

namespace simxr::app
{
TimelineGesture TimelineCanvas::Draw(const HistoryStore& history, bool following)
{
	TimelineGesture result;
	const auto top = ImGui::GetCursorScreenPos();
	const auto available = ImGui::GetContentRegionAvail();
	const float font = ImGui::GetFontSize(), width = std::max(1.f, available.x);
	const float height = std::max(1.f, available.y), labelWidth = std::min(width * .25f, font * 6);
	const float left = top.x + labelWidth, right = top.x + width;
	m_bounds = {left, top.y, right - left, height};
	const float pixels = std::max(1.f, right - left), lane = height / 5;
	ImGui::PushID(this);
	ImGui::InvisibleButton("Timeline canvas", {width, height},
						   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonMiddle);
	const bool hovered = ImGui::IsItemHovered(), active = ImGui::IsItemActive();
	auto* draw = ImGui::GetWindowDrawList();
	const auto muted = ImGui::GetColorU32(ImGuiCol_TextDisabled), line = ImGui::GetColorU32(ImGuiCol_Border);
	const auto accent = IM_COL32(47, 143, 163, 255);
	const char* labels[]{"Time", "Frames", "Images", "Recording", "Events"};
	for (int row = 0; row < 5; ++row)
	{
		const float y = top.y + row * lane;
		draw->AddLine({top.x, y}, {right, y}, line);
		if (lane >= font)
			draw->AddText({top.x + 4, y}, muted, labels[row]);
	}
	if (!history.Size())
	{
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
		const auto point = timeAt(io.MousePos.x);
		const auto ratio = (point - m_start) / m_span;
		m_span = std::clamp(m_span * std::pow(1.5, -io.MouseWheel), .01, std::max(10., latest - first));
		m_start = point - ratio * m_span;
		result.stopFollowing = true;
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
			result.marker = history.Nearest(timeAt(io.MousePos.x));
		if (ImGui::IsKeyPressed(ImGuiKey_End))
			result.goLive = true;
	}
	if (hovered && io.MousePos.x >= left && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
		m_dragStart = timeAt(io.MousePos.x);
	if (m_dragStart && ImGui::IsMouseReleased(ImGuiMouseButton_Left))
	{
		const auto end = timeAt(io.MousePos.x);
		result.first = history.Nearest(*m_dragStart);
		if (std::abs(end - *m_dragStart) * pixels / m_span > 5)
			result.last = history.Nearest(end);
		if (result.last && *result.first > *result.last)
			std::swap(result.first, result.last);
		result.stopFollowing = true;
		m_dragStart.reset();
	}
	draw->PushClipRect({left, top.y}, {right, top.y + height}, true);
	for (int column = 0; column < int(pixels); ++column)
	{
		const auto begin = history.LowerBound(m_start + column * m_span / pixels);
		const auto end = history.LowerBound(m_start + (column + 1) * m_span / pixels);
		const auto bucket = history.Query(begin, end);
		if (!bucket.count)
			continue;
		const float x = left + column;
		const auto color = bucket.flags & (HistoryLate | HistoryDropped) ? IM_COL32(229, 72, 77, 255) : accent;
		const auto bar = std::max(0.f, std::min(lane - 2, lane * bucket.maximum / 33.3f));
		draw->AddLine({x, top.y + 2 * lane - 1}, {x, top.y + 2 * lane - 1 - bar}, color);
		if (bucket.flags & (HistorySnapshot | HistoryPreview))
			draw->AddLine({x, top.y + 2 * lane + 2}, {x, top.y + 3 * lane - 2}, accent);
		if (bucket.flags & HistoryRecording)
			draw->AddLine({x, top.y + 3 * lane + 2}, {x, top.y + 4 * lane - 2}, IM_COL32(229, 72, 77, 255));
		if (bucket.flags & HistoryRunStart)
		{
			draw->AddLine({x, top.y}, {x, top.y + height}, muted);
			char label[48];
			std::snprintf(label, sizeof(label), "Run %llu",
						  static_cast<unsigned long long>(history.At(begin).identity.run));
			draw->AddText({x + 3, top.y + 4 * lane}, muted, label);
		}
		if (bucket.flags & HistoryGap)
			draw->AddCircle({x, top.y + 4.5f * lane}, 3, muted);
	}
	const auto visibleFirst = history.LowerBound(m_start), visibleEnd = history.LowerBound(m_start + m_span);
	if (visibleEnd > visibleFirst && pixels / (visibleEnd - visibleFirst) >= 6)
	{
		float previousLabel = left - 70;
		for (auto index = visibleFirst; index < visibleEnd; ++index)
		{
			const auto& frame = history.At(index);
			const float x = left + float((frame.seconds - m_start) / m_span) * pixels;
			draw->AddLine({x, top.y + lane - 4}, {x, top.y + lane}, muted);
			if (x - previousLabel < 64)
				continue;
			char text[32];
			std::snprintf(text, sizeof(text), "%llu", static_cast<unsigned long long>(frame.identity.frame));
			draw->AddText({x + 2, top.y}, muted, text);
			previousLabel = x;
		}
	}
	else
	{
		const double tick = std::pow(10., std::floor(std::log10(m_span / 8)));
		const double spacing = tick * (m_span / tick > 40 ? 5 : m_span / tick > 16 ? 2 : 1);
		for (double time = std::ceil(m_start / spacing) * spacing; time <= m_start + m_span; time += spacing)
		{
			if (time < first || time > latest)
				continue;
			const float x = left + float((time - m_start) / m_span) * pixels;
			char text[32];
			std::snprintf(text, sizeof(text), "%.2fs", time);
			draw->AddText({x + 3, top.y}, muted, text);
		}
	}
	if (m_dragStart)
	{
		const float x = left + float((*m_dragStart - m_start) / m_span) * pixels;
		draw->AddRectFilled({std::min(x, io.MousePos.x), top.y}, {std::max(x, io.MousePos.x), top.y + height},
							IM_COL32(47, 143, 163, 60));
	}
	draw->PopClipRect();
	if (hovered && io.MousePos.x >= left)
	{
		const auto nearest = history.Nearest(timeAt(io.MousePos.x));
		if (nearest)
		{
			const auto& frame = history.At(*nearest);
			ImGui::SetTooltip("Run %llu / frame %llu\nDisplay interval %.2f ms",
							  static_cast<unsigned long long>(frame.identity.run),
							  static_cast<unsigned long long>(frame.identity.frame), frame.frameMilliseconds);
		}
	}
	ImGui::PopID();
	return result;
}
bool TimelineCanvas::DrawMarker(double time, std::uint64_t frame)
{
	const float x = m_bounds.x + float((time - m_start) / m_span) * m_bounds.width;
	const float y = m_bounds.y + m_bounds.height * .9f;
	if (x < m_bounds.x || x > m_bounds.x + m_bounds.width)
		return false;
	auto* draw = ImGui::GetWindowDrawList();
	draw->AddTriangleFilled({x, y - 5}, {x - 5, y + 4}, {x + 5, y + 4}, IM_COL32(229, 181, 58, 255));
	const auto mouse = ImGui::GetIO().MousePos;
	if (std::abs(mouse.x - x) < 7 && std::abs(mouse.y - y) < 7 && ImGui::IsWindowHovered())
	{
		ImGui::SetTooltip("Marker / frame %llu", static_cast<unsigned long long>(frame));
		return ImGui::IsMouseReleased(ImGuiMouseButton_Left);
	}
	return false;
}
void TimelineCanvas::DrawEvent(double time, double observed, const char* text, bool failed)
{
	const float x = m_bounds.x + float((time - m_start) / m_span) * m_bounds.width;
	const float y = m_bounds.y + m_bounds.height * .9f;
	if (x < m_bounds.x || x > m_bounds.x + m_bounds.width)
		return;
	ImGui::GetWindowDrawList()->AddCircle({x, y}, 4, failed ? IM_COL32(229, 72, 77, 255) : IM_COL32(201, 167, 255, 255),
										  0, 2);
	const auto mouse = ImGui::GetIO().MousePos;
	if (std::abs(mouse.x - x) < 6 && std::abs(mouse.y - y) < 6 && ImGui::IsWindowHovered())
		ImGui::SetTooltip("%s\nObserved +%.2fs (nearest observed frame)", text, observed);
}
} // namespace simxr::app
