#include "app/widgets/InputPad.h"
#include "app/Theme.h"

#include <algorithm>
#include <cmath>
#include <cstdio>
#include <imgui_internal.h>
#include <string>

namespace simxr::app
{
namespace
{
float Unit()
{
	return ImGui::GetFontSize() / 17;
}
// Per-widget gesture memory: whether the current press became a drag.
bool& Dragged(ImGuiID id)
{
	static ImGuiID current = 0;
	static bool dragged = false;
	if (current != id)
	{
		current = id;
		dragged = false;
	}
	return dragged;
}
} // namespace
bool TriggerBar(const char* id, const char* label, float& value, bool& latched, ImU32 color, float width)
{
	const float unit = Unit(), height = 22 * unit;
	const auto minimum = ImGui::GetCursorScreenPos();
	const ImVec2 maximum{minimum.x + width, minimum.y + height};
	ImGui::InvisibleButton(id, {width, height}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_EnableNav);
	const auto previous = value;
	const auto& io = ImGui::GetIO();
	const float pointer = std::clamp((io.MousePos.x - minimum.x) / width, 0.f, 1.f);
	const auto item = ImGui::GetItemID();
	if (ImGui::IsItemActivated())
		Dragged(item) = false;
	if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3 * unit))
			Dragged(item) = true;
		value = Dragged(item) ? pointer : 1;
	}
	if (ImGui::IsItemDeactivated() && !latched)
		value = 0;
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
	{
		latched = !latched;
		value = latched ? std::max(pointer, value) : 0;
	}
	auto* draw = ImGui::GetWindowDrawList();
	const float radius = theme::Radius * unit;
	draw->AddRectFilled(minimum, maximum, theme::SurfaceRaised, radius);
	if (value > 0)
		draw->AddRectFilled(minimum, {minimum.x + width * value, maximum.y}, theme::Fade(color, .75f), radius);
	if (latched)
		draw->AddRect(minimum, maximum, color, radius, 0, 1.5f * unit);
	char text[48];
	std::snprintf(text, sizeof(text), "%s %d%%", label, static_cast<int>(std::lround(value * 100)));
	draw->AddText({minimum.x + 6 * unit, minimum.y + (height - ImGui::GetFontSize()) / 2}, theme::Text, text);
	ImGui::SetItemTooltip("%s: hold or drag; right-click %s", label, latched ? "releases" : "keeps it held");
	return value != previous;
}
bool HoldButton(const char* id, const char* label, bool& pressed, bool& latched, ImU32 color)
{
	const float unit = Unit(), size = 34 * unit;
	const auto minimum = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, {size, size}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_EnableNav);
	const auto previous = pressed;
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
		latched = !latched;
	pressed = latched || (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left));
	auto* draw = ImGui::GetWindowDrawList();
	const ImVec2 center{minimum.x + size / 2, minimum.y + size / 2};
	const float radius = size / 2 - unit;
	draw->AddCircleFilled(center, radius,
						  pressed					? color
						  : ImGui::IsItemHovered() ? theme::Mix(theme::SurfaceRaised, theme::Text, .12f)
												   : theme::SurfaceRaised);
	if (latched)
		draw->AddCircle(center, radius + .5f * unit, theme::Text, 0, 1.5f * unit);
	const auto text = ImGui::CalcTextSize(label, nullptr, true);
	draw->AddText({center.x - text.x / 2, center.y - text.y / 2}, pressed ? theme::Ink : theme::Text, label,
				  ImGui::FindRenderedTextEnd(label));
	const std::string name(label, ImGui::FindRenderedTextEnd(label));
	ImGui::SetItemTooltip("%s: hold to press; right-click %s", name.c_str(), latched ? "releases" : "keeps it pressed");
	return pressed != previous;
}
bool StickPad(const char* id, float& x, float& y, bool& click, bool& latched, ImU32 color)
{
	const float unit = Unit(), size = 96 * unit, radius = size / 2, knob = 14 * unit;
	const auto minimum = ImGui::GetCursorScreenPos();
	const ImVec2 center{minimum.x + radius, minimum.y + radius};
	ImGui::InvisibleButton(id, {size, size}, ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_EnableNav);
	const float previousX = x, previousY = y;
	const bool previousClick = click;
	const auto item = ImGui::GetItemID();
	const auto& io = ImGui::GetIO();
	if (ImGui::IsItemActivated())
		Dragged(item) = false;
	if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3 * unit))
			Dragged(item) = true;
		if (Dragged(item))
		{
			// OpenXR stick Y is up; screen Y is down.
			float dx = (io.MousePos.x - center.x) / (radius - knob), dy = (center.y - io.MousePos.y) / (radius - knob);
			const float length = std::hypot(dx, dy);
			if (length > 1)
			{
				dx /= length;
				dy /= length;
			}
			x = dx;
			y = dy;
		}
		click = !Dragged(item);
	}
	if (ImGui::IsItemDeactivated())
	{
		click = false;
		if (!latched)
			x = y = 0;
	}
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
	{
		latched = !latched;
		if (!latched)
			x = y = 0;
	}
	auto* draw = ImGui::GetWindowDrawList();
	draw->AddCircleFilled(center, radius, theme::SurfaceRaised);
	draw->AddLine({center.x - radius + 6 * unit, center.y}, {center.x + radius - 6 * unit, center.y}, theme::Line);
	draw->AddLine({center.x, center.y - radius + 6 * unit}, {center.x, center.y + radius - 6 * unit}, theme::Line);
	if (latched)
		draw->AddCircle(center, radius - unit, color, 0, 1.5f * unit);
	const ImVec2 at{center.x + x * (radius - knob), center.y - y * (radius - knob)};
	draw->AddCircleFilled(at, knob, click ? color : theme::Mix(theme::SurfaceRaised, color, .6f));
	draw->AddCircle(at, knob, color, 0, 1.5f * unit);
	ImGui::SetItemTooltip("Stick: drag; click the knob to press; right-click %s",
						  latched ? "releases" : "keeps it in place");
	return x != previousX || y != previousY || click != previousClick;
}
} // namespace simxr::app
