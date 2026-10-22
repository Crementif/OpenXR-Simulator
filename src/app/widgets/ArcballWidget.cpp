#include "Math.h"
#include "app/Theme.h"
#include "app/widgets/SpatialWidgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>
#include <numbers>

namespace simxr::app
{
SpatialEditResult ArcballWidget(const char* id, ArcballState& state, XrQuaternionf& orientation, XrQuaternionf view,
								XrQuaternionf reset, bool enabled, float size)
{
	if (state.dragging)
		view = state.view;
	const float scale = ImGui::GetFontSize() / 16;
	size *= scale;
	const auto minimum = ImGui::GetCursorScreenPos();
	const ImVec2 center{minimum.x + size / 2, minimum.y + size / 2};
	const float radius = size * .46f;
	ImGui::InvisibleButton(id, {size, size});
	const bool hovered = ImGui::IsItemHovered();
	const auto mouse = ImGui::GetIO().MousePos;
	const XrVector2f point{(mouse.x - center.x) / radius, (center.y - mouse.y) / radius};
	const float distance = std::sqrt(point.x * point.x + point.y * point.y);
	bool changed = false;
	auto* draw = ImGui::GetWindowDrawList();
	for (int ring = 24; ring > 0; --ring)
	{
		// Shaded rings make the ball read as a sphere.
		draw->AddCircleFilled(center, radius * ring / 24,
							  theme::Mix(theme::SurfaceRaised, theme::Text, (24 - ring) * .006f), 64);
	}
	draw->AddCircle(center, radius * .88f, theme::Fade(theme::TextDim, .6f), 64);
	const auto local = RelativePose({orientation, {}}, {view, {}}).orientation;
	const std::array axes{Rotate(local, {1, 0, 0}), Rotate(local, {0, 1, 0}), Rotate(local, {0, 0, -1})};
	const std::array colors{theme::AxisX, theme::AxisY, theme::AxisZ};
	int tip = -1;
	for (std::size_t axis = 0; axis < axes.size(); ++axis)
	{
		const auto direction = axes[axis];
		const ImVec2 end{center.x + direction.x * radius * .75f, center.y - direction.y * radius * .75f};
		const auto color = direction.z < 0 ? (colors[axis] & 0x00ffffff) | 0x60000000 : colors[axis];
		draw->AddLine(center, end, color, 2 * scale);
		draw->AddCircleFilled(end, 5 * scale, color);
		if (std::hypot(mouse.x - end.x, mouse.y - end.y) < 9 * scale)
			tip = static_cast<int>(axis);
	}
	// Axis legend in the lower-left corner.
	for (std::size_t axis = 0; axis < axes.size(); ++axis)
	{
		const char label[2] = {static_cast<char>('X' + axis), 0};
		draw->AddText({center.x - radius + axis * ImGui::GetFontSize() * .8f, center.y + radius - ImGui::GetFontSize()},
					  colors[axis], label);
	}
	if (enabled && hovered && distance <= 1 && ImGui::IsMouseDoubleClicked(ImGuiMouseButton_Left))
	{
		orientation = reset;
		state.dragging = false;
		return {true, true, false, true};
	}
	if (enabled && hovered && distance <= 1 && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		if (tip >= 0)
		{
			const auto axis = axes[tip];
			const auto delta = RotationBetween(axis, {0, 0, 1});
			orientation = Multiply(view, Multiply(delta, local));
			return {true, true, false, true};
		}
		state = {point, orientation, view, true, distance >= .88f};
	}
	if (state.dragging && enabled && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		XrQuaternionf delta;
		if (state.ring)
		{
			const float angle = std::atan2(point.y, point.x) - std::atan2(state.start.y, state.start.x);
			delta = EulerAngles(0, 0, angle);
		}
		else
			delta = ArcballRotation(state.start, point);
		if (ImGui::GetIO().KeyCtrl)
		{
			delta = SnapRotation(delta, std::numbers::pi_v<float> / 12);
			char angle[16];
			std::snprintf(angle, sizeof(angle), "%.0f\u00b0",
						  2 * std::acos(std::clamp(std::abs(delta.w), 0.0f, 1.0f)) * 180 / std::numbers::pi_v<float>);
			draw->AddText({center.x - ImGui::CalcTextSize(angle).x / 2, center.y - radius}, theme::Text, angle);
		}
		const auto start = RelativePose({state.orientation, {}}, {view, {}}).orientation;
		orientation = Multiply(view, Multiply(delta, start));
		changed = true;
	}
	const bool finished = state.dragging && (!enabled || !ImGui::IsMouseDown(ImGuiMouseButton_Left));
	if (finished)
		state.dragging = false;
	return {changed, finished, state.dragging, hovered && distance <= 1};
}
} // namespace simxr::app
