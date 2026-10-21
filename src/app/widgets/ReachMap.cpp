#include "Math.h"
#include "app/widgets/SpatialWidgets.h"
#include <algorithm>
#include <cmath>
#include <cstdio>

namespace simxr::app
{
SpatialEditResult ReachMap(const char* id, ReachMapState& state, XrPosef root, XrPosef head, float shoulderWidth,
						   float armReach, std::array<XrPosef, 2>& hands, const std::array<bool, 2>& editable,
						   float width)
{
	const float scale = ImGui::GetFontSize() / 16;
	const ImVec2 size{width * scale, 160 * scale};
	const auto minimum = ImGui::GetCursorScreenPos();
	const ImVec2 maximum{minimum.x + size.x, minimum.y + size.y};
	const ImVec2 center{minimum.x + (size.x - 28 * scale) / 2, minimum.y + size.y / 2};
	const float pixels = std::min(size.y * .44f, (size.x - 32 * scale) * .45f) / .9f;
	ImGui::InvisibleButton(id, size);
	const bool hovered = ImGui::IsItemHovered();
	auto* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(minimum, maximum, IM_COL32(22, 28, 35, 255), 5 * scale);
	draw->PushClipRect(minimum, maximum, true);
	for (float side : {-1.0f, 1.0f})
		draw->AddCircleFilled({center.x + side * shoulderWidth * .5f * pixels, center.y}, armReach * pixels,
							  IM_COL32(39, 51, 61, 120), 64);
	for (float radius : {.3f, .6f, .9f})
	{
		draw->AddCircle(center, radius * pixels, IM_COL32(90, 103, 119, 110), 64);
		char label[16];
		std::snprintf(label, sizeof(label), "%.1f m", radius);
		draw->AddText({center.x - radius * pixels * .7f - 8 * scale, center.y + radius * pixels * .7f - 8 * scale},
					  IM_COL32(125, 139, 154, 255), label);
	}
	const auto relativeHead = RelativePose(head, root);
	const float headHeight = relativeHead.position.y;
	const auto forward = Rotate(relativeHead.orientation, {0, 0, -1});
	draw->AddLine(center, {center.x + forward.x * 17 * scale, center.y + forward.z * 17 * scale},
				  IM_COL32(213, 222, 231, 255), 2 * scale);
	std::array<XrPosef, 2> offsets{RelativePose(hands[0], root), RelativePose(hands[1], root)};
	std::array<ImVec2, 2> dots;
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		dots[hand] = {
			std::clamp(center.x + offsets[hand].position.x * pixels, minimum.x + 10 * scale, maximum.x - 38 * scale),
			std::clamp(center.y + offsets[hand].position.z * pixels, minimum.y + 10 * scale, maximum.y - 10 * scale)};
		const auto color = DeviceColor(static_cast<int>(hand) + 1, editable[hand] ? 1 : .4f);
		draw->AddCircleFilled(dots[hand], 9 * scale, color);
		draw->AddText({dots[hand].x - 4 * scale, dots[hand].y - 7 * scale}, IM_COL32(15, 20, 25, 255),
					  hand ? "R" : "L");
		if (state.hand == static_cast<int>(hand))
			draw->AddCircle(dots[hand], 12 * scale, color);
	}
	const float barX = maximum.x - 16 * scale, barTop = minimum.y + 12 * scale, barBottom = maximum.y - 12 * scale;
	const float relativeHeight = offsets[state.hand].position.y - headHeight;
	const float knobY = std::clamp(barTop + (.8f - relativeHeight) / 2 * (barBottom - barTop), barTop, barBottom);
	draw->AddLine({barX, barTop}, {barX, barBottom}, IM_COL32(103, 116, 130, 255), 5 * scale);
	draw->AddLine({barX - 8 * scale, barTop + .4f * (barBottom - barTop)},
				  {barX + 8 * scale, barTop + .4f * (barBottom - barTop)}, IM_COL32(180, 190, 202, 255));
	draw->AddCircleFilled({barX, knobY}, 7 * scale, DeviceColor(state.hand + 1));
	const auto mouse = ImGui::GetIO().MousePos;
	if (hovered && ImGui::IsMouseClicked(ImGuiMouseButton_Left))
	{
		state.height = std::abs(mouse.x - barX) < 12 * scale;
		if (state.height)
			state.dragging = editable[state.hand];
		else
			for (std::size_t hand = 0; hand < 2; ++hand)
				if (editable[hand] && std::hypot(mouse.x - dots[hand].x, mouse.y - dots[hand].y) < 13 * scale)
				{
					state.hand = static_cast<int>(hand);
					state.dragging = true;
				}
	}
	bool changed = false;
	if (state.dragging && editable[state.hand] && ImGui::IsMouseDown(ImGuiMouseButton_Left))
	{
		auto& offset = offsets[state.hand];
		if (state.height)
			offset.position.y =
				headHeight + .8f - 2 * std::clamp((mouse.y - barTop) / (barBottom - barTop), 0.0f, 1.0f);
		else
		{
			offset.position.x = (mouse.x - center.x) / pixels;
			offset.position.z = (mouse.y - center.y) / pixels;
		}
		hands[state.hand] = Compose(root, offset);
		changed = true;
	}
	const bool finished = state.dragging && (!editable[state.hand] || !ImGui::IsMouseDown(ImGuiMouseButton_Left));
	if (finished)
		state.dragging = false;
	if (hovered)
		ImGui::SetTooltip("Hand height: %+.2f m", relativeHeight);
	draw->PopClipRect();
	return {changed, finished, state.dragging, hovered};
}
} // namespace simxr::app
