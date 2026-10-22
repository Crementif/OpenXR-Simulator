#include "app/widgets/Controls.h"
#include "app/Theme.h"

#include <algorithm>
#include <imgui_internal.h>

namespace simxr::app
{
bool IconButton(const char* id, Icon icon, const char* tooltip, bool toggled, const char* label, ImU32 tint)
{
	const float height = ImGui::GetFrameHeight(), padding = ImGui::GetStyle().FramePadding.x;
	const float textWidth = label ? ImGui::CalcTextSize(label, nullptr, true).x : 0;
	const ImVec2 size{label ? height + textWidth + padding : height, height};
	const auto minimum = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(id, size, ImGuiButtonFlags_EnableNav);
	const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
	const bool disabled = ImGui::GetItemFlags() & ImGuiItemFlags_Disabled;
	const ImVec2 maximum{minimum.x + size.x, minimum.y + size.y};
	auto* draw = ImGui::GetWindowDrawList();
	const auto background = held		 ? theme::Mix(theme::SurfaceRaised, theme::Accent, .55f)
							: toggled ? theme::Mix(theme::SurfaceRaised, theme::Accent, .4f)
							: hovered ? theme::Mix(theme::SurfaceRaised, theme::Text, .1f)
									  : theme::SurfaceRaised;
	draw->AddRectFilled(minimum, maximum, background, theme::Radius * ImGui::GetFontSize() / 17);
	auto color = tint ? tint : theme::Text;
	if (disabled)
		color = theme::Fade(color, .4f);
	DrawIcon(draw, icon, {minimum.x + height / 2, minimum.y + height / 2}, height * .6f, color);
	if (label)
		draw->AddText({minimum.x + height - padding / 2, minimum.y + (height - ImGui::GetFontSize()) / 2},
					  disabled ? theme::TextDim : theme::Text, label, ImGui::FindRenderedTextEnd(label));
	if (tooltip)
		ImGui::SetItemTooltip("%s", tooltip);
	return pressed;
}
bool StateChip(const char* id, ImU32 dot, const char* label, const char* tooltip, const Icon* action,
			   const char* actionTooltip)
{
	ImGui::PushID(id);
	const float height = ImGui::GetFrameHeight(), unit = ImGui::GetFontSize() / 17;
	const auto textSize = ImGui::CalcTextSize(label);
	const float actionWidth = action ? height - 4 * unit : 0;
	const float width = 22 * unit + textSize.x + (action ? actionWidth + 4 * unit : 10 * unit);
	const auto minimum = ImGui::GetCursorScreenPos();
	auto* draw = ImGui::GetWindowDrawList();
	draw->AddRectFilled(minimum, {minimum.x + width, minimum.y + height}, theme::SurfaceRaised,
						theme::ChipRadius * unit);
	draw->AddCircleFilled({minimum.x + 12 * unit, minimum.y + height / 2}, 4 * unit, dot);
	draw->AddText({minimum.x + 22 * unit, minimum.y + (height - textSize.y) / 2}, theme::Text, label);
	ImGui::InvisibleButton("##Chip", {width - actionWidth - (action ? 4 * unit : 0), height});
	if (tooltip)
		ImGui::SetItemTooltip("%s", tooltip);
	bool pressed = false;
	if (action)
	{
		ImGui::SameLine(0, 0);
		const auto start = ImGui::GetCursorScreenPos();
		ImGui::SetCursorScreenPos({start.x, minimum.y + 2 * unit});
		const float side = actionWidth;
		pressed = ImGui::InvisibleButton("##Action", {side, side}, ImGuiButtonFlags_EnableNav);
		const auto center = ImVec2{start.x + side / 2, minimum.y + 2 * unit + side / 2};
		if (ImGui::IsItemHovered())
			draw->AddCircleFilled(center, side / 2, theme::Mix(theme::SurfaceRaised, theme::Text, .15f));
		DrawIcon(draw, *action, center, side * .55f, theme::Text);
		if (actionTooltip)
			ImGui::SetItemTooltip("%s", actionTooltip);
		ImGui::SameLine(0, 4 * unit);
		ImGui::Dummy({0, height});
	}
	ImGui::PopID();
	return pressed;
}
bool RailItem(const char* label, Icon icon, bool selected, const RailStatus& status, float height)
{
	const float unit = ImGui::GetFontSize() / 17;
	if (height <= 0)
		height = 36 * unit;
	const float width = std::max(ImGui::GetContentRegionAvail().x, height);
	const auto minimum = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(label, {width, height}, ImGuiButtonFlags_EnableNav);
	const bool hovered = ImGui::IsItemHovered();
	auto* draw = ImGui::GetWindowDrawList();
	const ImVec2 maximum{minimum.x + width, minimum.y + height};
	if (selected || hovered)
		draw->AddRectFilled(minimum, maximum,
							selected ? theme::SurfaceRaised : theme::Mix(theme::Surface, theme::Text, .05f),
							theme::Radius * unit);
	if (selected)
		draw->AddRectFilled(minimum, {minimum.x + 3 * unit, maximum.y}, theme::Accent, 1.5f * unit);
	const float iconSize = 18 * unit, iconX = minimum.x + 8 * unit + iconSize / 2;
	DrawIcon(draw, icon, {iconX, minimum.y + height / 2}, iconSize, selected ? theme::Text : theme::TextDim);
	if (status.dot)
		draw->AddCircleFilled({iconX + iconSize / 2, minimum.y + height / 2 + iconSize / 2 - 2 * unit}, 3.5f * unit,
							  status.dot);
	const float textX = iconX + iconSize / 2 + 8 * unit;
	// An icon-only rail names the row in its tooltip.
	const bool named = !status.iconOnly;
	if (named)
		draw->AddText({textX, minimum.y + (height - ImGui::GetFontSize()) / 2}, selected ? theme::Text : theme::TextDim,
					  label);
	float right = maximum.x - 6 * unit;
	const float textEnd = textX + ImGui::CalcTextSize(label).x + 6 * unit;
	for (const auto& [text, color] : {std::pair{status.key, theme::TextDim}, {status.badge, status.badgeColor}})
	{
		if (!text)
			continue;
		const auto size = ImGui::CalcTextSize(text);
		if (!named || right - size.x < textEnd)
			break;
		draw->AddText({right - size.x, minimum.y + (height - size.y) / 2}, color, text);
		right -= size.x + 8 * unit;
	}
	if (hovered && (status.dotTooltip || status.key || !named))
	{
		ImGui::BeginTooltip();
		ImGui::TextUnformatted(label);
		if (status.dotTooltip)
			ImGui::TextDisabled("%s", status.dotTooltip);
		if (status.key)
			ImGui::TextDisabled("Pilot with %s", status.key);
		ImGui::EndTooltip();
	}
	return pressed;
}
} // namespace simxr::app
