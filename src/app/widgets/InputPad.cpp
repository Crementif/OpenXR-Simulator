#include "app/widgets/InputPad.h"
#include "app/Theme.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdio>
#include <imgui_internal.h>
#include <numbers>
#include <string>

namespace simxr::app
{
namespace
{
float Unit()
{
	return ImGui::GetFontSize() / 17;
}
// The gesture of the input the mouse holds; only one can be held at a time.
struct Gesture
{
	ImGuiID id = 0;
	bool dragged = false;
	// The stick arrow the press started on (0 right, 1 up, 2 left, 3 down), or -1.
	int direction = -1;
};
Gesture& Held(ImGuiID id)
{
	static Gesture s_gesture;
	if (s_gesture.id != id)
		s_gesture = {id};
	return s_gesture;
}
// A per-item display value that eases toward target, so presses and releases animate.
float Ease(ImGuiID item, const char* name, float target, float speed)
{
	float& value = *ImGui::GetStateStorage()->GetFloatRef(ImHashStr(name, 0, item), target);
	value += (target - value) * std::min(1.0f, ImGui::GetIO().DeltaTime * speed);
	if (std::abs(target - value) < 1e-3f)
		value = target;
	return value;
}
} // namespace
bool TriggerBar(const char* id, const char* label, float& value, bool& latched, ImU32 color, float width)
{
	const float unit = Unit(), height = 24 * unit;
	const auto minimum = ImGui::GetCursorScreenPos();
	const ImVec2 maximum{minimum.x + width, minimum.y + height};
	ImGui::InvisibleButton(id, {width, height},
						   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_EnableNav);
	const auto previous = value;
	const auto& io = ImGui::GetIO();
	const float pointer = std::clamp((io.MousePos.x - minimum.x) / width, 0.f, 1.f);
	const auto item = ImGui::GetItemID();
	if (ImGui::IsItemActivated())
		Held(item).dragged = false;
	if (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, 3 * unit))
			Held(item).dragged = true;
		value = Held(item).dragged ? pointer : 1;
	}
	if (ImGui::IsItemDeactivated() && !latched)
		value = 0;
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
	{
		latched = !latched;
		value = latched ? std::max(pointer, value) : 0;
	}
	const bool held = ImGui::IsItemActive();
	const float shown = Ease(item, "shown", value, 24);
	auto* draw = ImGui::GetWindowDrawList();
	const float radius = theme::Radius * unit;
	draw->AddRectFilled(minimum, maximum,
						ImGui::IsItemHovered() ? theme::Mix(theme::SurfaceRaised, theme::Text, .06f) : theme::SurfaceRaised,
						radius);
	// A sliver under a pixel would draw as a rounded blob.
	if (shown * width >= 1)
	{
		const float end = minimum.x + width * shown;
		draw->AddRectFilled(minimum, {end, maximum.y}, theme::Fade(color, held ? .9f : .7f), radius);
		// A bright edge where the value ends.
		if (shown < 1)
			draw->AddLine({end - unit, minimum.y + 3 * unit}, {end - unit, maximum.y - 3 * unit},
						  theme::Mix(color, theme::Text, .6f), 2 * unit);
	}
	if (latched)
		draw->AddRect(minimum, maximum, color, radius, 0, 1.5f * unit);
	const float textY = minimum.y + (height - ImGui::GetFontSize()) / 2;
	draw->AddText({minimum.x + 8 * unit, textY}, theme::Text, label);
	char percent[8];
	std::snprintf(percent, sizeof(percent), "%d%%", static_cast<int>(std::lround(value * 100)));
	draw->AddText({maximum.x - 8 * unit - ImGui::CalcTextSize(percent).x, textY},
				  value > 0 ? theme::Text : theme::TextDim, percent);
	ImGui::SetItemTooltip("%s: hold or drag; right-click %s", label, latched ? "releases" : "keeps it held");
	return value != previous;
}
bool HoldButton(const char* id, const char* label, bool& pressed, bool& latched, ImU32 color)
{
	const float unit = Unit(), size = 34 * unit;
	const auto minimum = ImGui::GetCursorScreenPos();
	ImGui::InvisibleButton(id, {size, size},
						   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_EnableNav);
	const auto previous = pressed;
	if (ImGui::IsItemClicked(ImGuiMouseButton_Right))
		latched = !latched;
	pressed = latched || (ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left));
	// The button sinks and fills with the hand's colour while held, and rises again on release.
	const float press = Ease(ImGui::GetItemID(), "press", pressed ? 1.0f : 0.0f, 22);
	auto* draw = ImGui::GetWindowDrawList();
	const ImVec2 center{minimum.x + size / 2, minimum.y + size / 2};
	const float radius = size / 2 - unit, face = radius * (1 - .08f * press);
	if (press > 0)
		draw->AddCircleFilled(center, radius + unit * press, theme::Fade(color, .3f * press));
	const auto rest = ImGui::IsItemHovered() ? theme::Mix(theme::SurfaceRaised, theme::Text, .12f) : theme::SurfaceRaised;
	draw->AddCircleFilled(center, face, theme::Mix(rest, color, press));
	draw->AddCircle(center, face, theme::Mix(theme::Line, color, press), 0, unit);
	if (latched)
		draw->AddCircle(center, radius + .5f * unit, theme::Text, 0, 1.5f * unit);
	const auto text = ImGui::CalcTextSize(label, nullptr, true);
	draw->AddText({center.x - text.x / 2, center.y - text.y / 2}, theme::Mix(theme::Text, theme::Ink, press), label,
				  ImGui::FindRenderedTextEnd(label));
	const std::string name(label, ImGui::FindRenderedTextEnd(label));
	ImGui::SetItemTooltip("%s: hold to press; right-click %s", name.c_str(), latched ? "releases" : "keeps it pressed");
	return pressed != previous;
}
bool StickPad(const char* id, float& x, float& y, bool& click, bool& latched, ImU32 color)
{
	constexpr float Pi = std::numbers::pi_v<float>;
	constexpr std::array<ImVec2, 4> Directions{{{1, 0}, {0, 1}, {-1, 0}, {0, -1}}};
	const float unit = Unit(), size = 96 * unit, radius = size / 2, knob = 14 * unit, travel = radius - knob;
	const auto minimum = ImGui::GetCursorScreenPos();
	const ImVec2 center{minimum.x + radius, minimum.y + radius};
	ImGui::InvisibleButton(id, {size, size},
						   ImGuiButtonFlags_MouseButtonLeft | ImGuiButtonFlags_MouseButtonRight | ImGuiButtonFlags_EnableNav);
	const float previousX = x, previousY = y;
	const bool previousClick = click;
	const auto item = ImGui::GetItemID();
	const auto& io = ImGui::GetIO();
	// OpenXR stick Y is up; screen Y is down.
	const float pointerX = io.MousePos.x - center.x, pointerY = center.y - io.MousePos.y;
	const auto arrowAt = [&]
	{
		if (std::hypot(pointerX, pointerY) <= knob + 4 * unit)
			return -1;
		return static_cast<int>(std::lround(std::atan2(pointerY, pointerX) / (Pi / 2))) & 3;
	};
	if (ImGui::IsItemActivated())
		Held(item) = {item, false, arrowAt()};
	const bool holding = ImGui::IsItemActive() && ImGui::IsMouseDown(ImGuiMouseButton_Left);
	auto& gesture = Held(item);
	if (holding)
	{
		// A press on an arrow keeps it pushed; only a clear drag turns it into steering.
		if (ImGui::IsMouseDragging(ImGuiMouseButton_Left, (gesture.direction < 0 ? 3 : 12) * unit))
			gesture.dragged = true;
		if (gesture.dragged)
		{
			float dx = pointerX / travel, dy = pointerY / travel;
			const float length = std::hypot(dx, dy);
			if (length > 1)
			{
				dx /= length;
				dy /= length;
			}
			x = dx;
			y = dy;
		}
		else if (gesture.direction >= 0)
		{
			x = Directions[gesture.direction].x;
			y = Directions[gesture.direction].y;
		}
		click = gesture.direction < 0 && !gesture.dragged;
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
	// The knob follows the pointer exactly and eases back to the centre on release.
	const float speed = holding && gesture.dragged ? 1000.0f : 18.0f;
	const float shownX = Ease(item, "x", x, speed), shownY = Ease(item, "y", y, speed);
	auto* draw = ImGui::GetWindowDrawList();
	draw->AddCircleFilled(center, radius, theme::SurfaceRaised, 48);
	draw->AddCircle(center, radius, theme::Line, 48, unit);
	const int hoveredArrow = ImGui::IsItemHovered() && !holding ? arrowAt() : -2;
	const int pushedArrow = holding && !gesture.dragged ? gesture.direction : -2;
	for (int arrow = 0; arrow < 4; ++arrow)
	{
		const float angle = -arrow * Pi / 2;
		if (arrow == hoveredArrow || arrow == pushedArrow)
		{
			draw->PathArcTo(center, radius - unit, angle - Pi / 4, angle + Pi / 4, 12);
			draw->PathArcTo(center, knob + 4 * unit, angle + Pi / 4, angle - Pi / 4, 8);
			draw->PathFillConcave(arrow == pushedArrow ? theme::Fade(color, .45f) : theme::Fade(theme::Text, .07f));
		}
		// Quiet separators between the arrows make it read as a pad.
		const ImVec2 diagonal{std::cos(angle + Pi / 4), std::sin(angle + Pi / 4)};
		draw->AddLine({center.x + diagonal.x * (knob + 6 * unit), center.y + diagonal.y * (knob + 6 * unit)},
					  {center.x + diagonal.x * (radius - 4 * unit), center.y + diagonal.y * (radius - 4 * unit)},
					  theme::Line, unit);
		const ImVec2 direction{std::cos(angle), std::sin(angle)}, across{-direction.y, direction.x};
		const float tip = radius - 8 * unit, depth = 5 * unit, half = 6 * unit;
		const ImVec2 point{center.x + direction.x * tip, center.y + direction.y * tip};
		const std::array<ImVec2, 3> chevron{
			ImVec2{point.x - direction.x * depth + across.x * half, point.y - direction.y * depth + across.y * half},
			point,
			ImVec2{point.x - direction.x * depth - across.x * half, point.y - direction.y * depth - across.y * half}};
		draw->AddPolyline(chevron.data(), 3, arrow == pushedArrow || arrow == hoveredArrow ? theme::Text : theme::TextDim,
						  ImDrawFlags_None, 2 * unit);
	}
	if (latched)
		draw->AddCircle(center, radius - unit, color, 48, 1.5f * unit);
	const ImVec2 at{center.x + shownX * travel, center.y - shownY * travel};
	draw->AddCircleFilled({at.x, at.y + unit}, knob + unit, theme::Fade(theme::Ink, .6f));
	draw->AddCircleFilled(at, knob, click ? color : theme::Mix(theme::SurfaceRaised, color, .6f));
	draw->AddCircle(at, knob, color, 0, 1.5f * unit);
	ImGui::SetItemTooltip("Stick: click an arrow to push it, drag to steer, click the knob to press; right-click %s",
						  latched ? "releases" : "keeps it in place");
	return x != previousX || y != previousY || click != previousClick;
}
} // namespace simxr::app
