#include "app/widgets/Controls.h"
#include "Pch.h"
#include "app/widgets/Theme.h"

#include <cfloat>
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
	const ImVec2 maximum{minimum.x + size.x, minimum.y + size.y};
	auto* draw = ImGui::GetWindowDrawList();
	const auto background = held	  ? theme::Vec(theme::Mix(theme::SurfaceRaised, theme::Accent, .55f))
							: toggled ? theme::Vec(theme::Mix(theme::SurfaceRaised, theme::Accent, .4f))
							: hovered ? theme::Vec(theme::Mix(theme::SurfaceRaised, theme::Text, .1f))
									  : ImGui::GetStyle().Colors[ImGuiCol_Button];
	draw->AddRectFilled(minimum, maximum, ImGui::GetColorU32(background), theme::Radius * ImGui::GetFontSize() / 17);
	DrawIcon(draw, icon, {minimum.x + height / 2, minimum.y + height / 2}, height * .6f,
			 ImGui::GetColorU32(tint ? tint : theme::Text));
	if (label)
		draw->AddText({minimum.x + height - padding / 2, minimum.y + (height - ImGui::GetFontSize()) / 2},
					  ImGui::GetColorU32(theme::Text), label, ImGui::FindRenderedTextEnd(label));
	if (tooltip)
		ImGui::SetItemTooltip("%s", tooltip);
	return pressed;
}
int Segmented(std::span<const char* const> labels, int selected, float width, std::span<const char* const> tooltips)
{
	const int count = static_cast<int>(labels.size());
	const float height = ImGui::GetFrameHeight(), radius = theme::Radius * ImGui::GetFontSize() / 17;
	const auto origin = ImGui::GetCursorScreenPos();
	auto* draw = ImGui::GetWindowDrawList();
	// GetColorU32 applies the disabled alpha.
	draw->AddRectFilled(origin, {origin.x + width, origin.y + height}, ImGui::GetColorU32(theme::SurfaceRaised),
						radius);
	int clicked = -1;
	for (int index = 0; index < count; ++index)
	{
		const float left = origin.x + width * index / count, right = origin.x + width * (index + 1) / count;
		ImGui::SetCursorScreenPos({left, origin.y});
		if (ImGui::InvisibleButton(labels[index], {right - left, height}, ImGuiButtonFlags_EnableNav))
			clicked = index;
		const bool hovered = ImGui::IsItemHovered(), held = ImGui::IsItemActive();
		const auto corners = count == 1			  ? ImDrawFlags_RoundCornersAll
							 : index == 0		  ? ImDrawFlags_RoundCornersLeft
							 : index == count - 1 ? ImDrawFlags_RoundCornersRight
												  : ImDrawFlags_RoundCornersNone;
		if (index == selected || hovered)
			draw->AddRectFilled({left, origin.y}, {right, origin.y + height},
								ImGui::GetColorU32(index == selected ? theme::Accent
																	 : theme::Mix(theme::SurfaceRaised, theme::Text,
																				  held ? .16f : .08f)),
								radius, corners);
		else if (index && index - 1 != selected)
			draw->AddLine({left, origin.y + height * .25f}, {left, origin.y + height * .75f},
						  ImGui::GetColorU32(theme::Line));
		const char* end = ImGui::FindRenderedTextEnd(labels[index]);
		const auto text = ImGui::CalcTextSize(labels[index], end);
		draw->AddText({(left + right - text.x) / 2, origin.y + (height - text.y) / 2}, ImGui::GetColorU32(theme::Text),
					  labels[index], end);
		if (index < static_cast<int>(tooltips.size()) && tooltips[index])
			ImGui::SetItemTooltip("%s", tooltips[index]);
	}
	ImGui::SetCursorScreenPos(origin);
	ImGui::Dummy({width, height});
	return clicked;
}
void PageTitle(Icon icon, const char* title)
{
	const float height = ImGui::GetFrameHeight(), size = ImGui::GetFontSize() * 1.12f;
	const float textX = height + ImGui::GetStyle().ItemSpacing.x / 2;
	const auto origin = ImGui::GetCursorScreenPos();
	auto* draw = ImGui::GetWindowDrawList();
	DrawIcon(draw, icon, {origin.x + height / 2, origin.y + height / 2}, height * .7f, ImGui::GetColorU32(theme::Text));
	draw->AddText(ImGui::GetFont(), size, {origin.x + textX, origin.y + (height - size) / 2},
				  ImGui::GetColorU32(theme::Text), title);
	ImGui::Dummy({textX + ImGui::CalcTextSize(title).x * 1.12f, height});
}
bool PageClose()
{
	ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetContentRegionMax().x - ImGui::GetFrameHeight()));
	return IconButton("Close page", Icon::Close, "Back to the device page (Esc)");
}
static std::string Capitals(const char* label)
{
	std::string text = label;
	for (auto& character : text)
		if (character >= 'a' && character <= 'z')
			character = static_cast<char>(character - 'a' + 'A');
	return text;
}
// Room above a section, except at the top of its window.
static void SectionGap()
{
	if (ImGui::GetCursorPosY() > ImGui::GetCursorStartPos().y + 1)
		ImGui::Dummy({0, ImGui::GetFontSize() * .3f});
}
void FieldLabel(const char* label)
{
	ImGui::AlignTextToFramePadding();
	ImGui::TextColored(theme::Vec(theme::TextLabel), "%s", label);
	ImGui::SameLine(std::max(ImGui::GetCursorPosX(), ImGui::GetFontSize() * 6.5f));
	ImGui::SetNextItemWidth(-FLT_MIN);
}
void SectionLabel(const char* label)
{
	SectionGap();
	ImGui::PushFont(nullptr, ImGui::GetFontSize() * .85f);
	ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
	ImGui::TextUnformatted(Capitals(label).c_str());
	ImGui::PopStyleColor();
	ImGui::PopFont();
}
bool Section(const char* label, ImGuiTreeNodeFlags flags)
{
	SectionGap();
	ImGui::PushFont(nullptr, ImGui::GetFontSize() * .85f);
	ImGui::PushStyleColor(ImGuiCol_Text, theme::TextDim);
	ImGui::PushStyleColor(ImGuiCol_HeaderHovered, theme::Fade(theme::Text, .05f));
	ImGui::PushStyleColor(ImGuiCol_HeaderActive, theme::Fade(theme::Text, .08f));
	flags |=
		ImGuiTreeNodeFlags_NoTreePushOnOpen | ImGuiTreeNodeFlags_SpanAvailWidth | ImGuiTreeNodeFlags_NoAutoOpenOnLog;
	const bool open = ImGui::TreeNodeEx(label, flags, "%s", Capitals(label).c_str());
	ImGui::PopStyleColor(3);
	ImGui::PopFont();
	return open;
}
static ImU32 IconColor(const RailStatus& status, bool selected)
{
	if (status.tint)
		return ImGui::GetColorU32(selected ? status.tint : theme::Fade(status.tint, .7f));
	return ImGui::GetColorU32(selected ? theme::Text : theme::TextDim);
}
static void TabTooltip(const char* label, const RailStatus& status)
{
	ImGui::BeginTooltip();
	ImGui::TextUnformatted(label);
	if (status.dotTooltip)
		ImGui::TextDisabled("%s", status.dotTooltip);
	if (status.badge)
		ImGui::TextDisabled("Driven by %s", status.badge);
	if (status.key)
		ImGui::TextDisabled(status.pilotKey ? "Pilot with %s" : "%s", status.key);
	ImGui::EndTooltip();
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
	if (hovered || ImGui::IsItemActive())
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	// The rail's window padding is crossed so a selected tab meets the inspector.
	const float join = ImGui::GetCurrentWindowRead()->WindowPadding.x;
	if (selected)
	{
		const auto window = ImGui::GetWindowPos(), size = ImGui::GetWindowSize();
		draw->PushClipRect(window, {window.x + size.x, window.y + size.y});
		draw->AddRectFilled(minimum, {maximum.x + join, maximum.y}, ImGui::GetColorU32(theme::Surface),
							theme::Radius * unit, ImDrawFlags_RoundCornersLeft);
		draw->PopClipRect();
		draw->AddRectFilled(minimum, {minimum.x + 3 * unit, maximum.y}, ImGui::GetColorU32(theme::Accent), 1.5f * unit);
	}
	else if (hovered)
		draw->AddRectFilled(minimum, maximum, ImGui::GetColorU32(theme::Mix(theme::Bg, theme::Text, .07f)),
							theme::Radius * unit);
	const float iconSize = 18 * unit, iconX = minimum.x + 8 * unit + iconSize / 2;
	DrawIcon(draw, icon, {iconX, minimum.y + height / 2}, iconSize, IconColor(status, selected));
	if (status.dot)
		draw->AddCircleFilled({iconX + iconSize / 2, minimum.y + height / 2 + iconSize / 2 - 2 * unit}, 3.5f * unit,
							  ImGui::GetColorU32(status.dot));
	const float textX = iconX + iconSize / 2 + 8 * unit;
	// An icon-only rail names the row in its tooltip.
	const bool named = !status.iconOnly;
	if (named)
		draw->AddText({textX, minimum.y + (height - ImGui::GetFontSize()) / 2},
					  ImGui::GetColorU32(selected ? theme::Text : theme::TextDim), label);
	float right = maximum.x - 6 * unit;
	const float textEnd = textX + ImGui::CalcTextSize(label).x + 6 * unit;
	for (const auto& [text, color] : {std::pair{status.key, theme::TextDim}, {status.badge, status.badgeColor}})
	{
		if (!text)
			continue;
		const auto size = ImGui::CalcTextSize(text);
		if (!named || right - size.x < textEnd)
			break;
		draw->AddText({right - size.x, minimum.y + (height - size.y) / 2}, ImGui::GetColorU32(color), text);
		right -= size.x + 8 * unit;
	}
	if (hovered && (status.dotTooltip || status.key || !named))
		TabTooltip(label, status);
	return pressed;
}
bool DeviceTab(const char* label, Icon icon, bool selected, const RailStatus& status, ImVec2 size, bool named)
{
	const float unit = ImGui::GetFontSize() / 17;
	const auto minimum = ImGui::GetCursorScreenPos();
	const bool pressed = ImGui::InvisibleButton(label, size, ImGuiButtonFlags_EnableNav);
	const bool hovered = ImGui::IsItemHovered();
	if (hovered || ImGui::IsItemActive())
		ImGui::SetMouseCursor(ImGuiMouseCursor_Hand);
	auto* draw = ImGui::GetWindowDrawList();
	const ImVec2 maximum{minimum.x + size.x, minimum.y + size.y};
	if (hovered && !selected)
		draw->AddRectFilled({minimum.x + 2 * unit, minimum.y + 4 * unit}, {maximum.x - 2 * unit, maximum.y - 5 * unit},
							ImGui::GetColorU32(theme::Mix(theme::Surface, theme::Text, .06f)), theme::Radius * unit);
	const float iconSize = 18 * unit;
	const auto text = ImGui::CalcTextSize(label);
	const float content = named ? iconSize + 6 * unit + text.x : iconSize;
	const float iconX = minimum.x + (size.x - content) / 2 + iconSize / 2;
	const float centerY = minimum.y + (size.y - 2 * unit) / 2;
	DrawIcon(draw, icon, {iconX, centerY}, iconSize, IconColor(status, selected));
	// Status sits on the icon's lower corner, owner on its upper one; a ring of the strip's colour keeps them apart.
	if (status.dot)
	{
		const ImVec2 dot{iconX + iconSize / 2 - unit, centerY + iconSize / 2 - 2 * unit};
		draw->AddCircleFilled(dot, 5 * unit, ImGui::GetColorU32(theme::Surface));
		draw->AddCircleFilled(dot, 3.5f * unit, ImGui::GetColorU32(status.dot));
	}
	if (status.badge)
	{
		const ImVec2 badge{iconX + iconSize / 2 - unit, centerY - iconSize / 2 + 2 * unit};
		draw->AddCircleFilled(badge, 5 * unit, ImGui::GetColorU32(theme::Surface));
		draw->AddCircleFilled(badge, 3.5f * unit, ImGui::GetColorU32(status.badgeColor));
	}
	if (named)
		draw->AddText({iconX + iconSize / 2 + 6 * unit, centerY - text.y / 2}, ImGui::GetColorU32(theme::Text), label);
	if (selected)
		draw->AddRectFilled({minimum.x + 6 * unit, maximum.y - 3 * unit}, {maximum.x - 6 * unit, maximum.y - unit},
							ImGui::GetColorU32(theme::Accent), unit);
	if (hovered)
		TabTooltip(label, status);
	return pressed;
}
} // namespace simxr::app
