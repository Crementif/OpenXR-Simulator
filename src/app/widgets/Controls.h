#pragma once

#include "app/widgets/Icons.h"

#include <imgui.h>
#include <span>

namespace simxr::app
{
// A square icon button, or icon plus word when label is set. The id stays
// stable across label changes; the tooltip is required (§8.2).
bool IconButton(const char* id, Icon icon, const char* tooltip, bool toggled = false, const char* label = nullptr,
				ImU32 tint = 0);
// Joined buttons of equal width across width; the selected one (or none, -1) is accented.
// Each label is its button's id in the current ID scope. Returns the clicked index, or -1.
int Segmented(std::span<const char* const> labels, int selected, float width,
			  std::span<const char* const> tooltips = {});
// A rounded pill: coloured dot, label and an optional action button inside the
// pill. Returns true when the action was pressed.
bool StateChip(const char* id, ImU32 dot, const char* label, const char* tooltip = nullptr,
			   const Icon* action = nullptr, const char* actionTooltip = nullptr);
// What a rail row shows besides its name.
struct RailStatus
{
	// 0 draws no dot.
	ImU32 dot = 0;
	const char* dotTooltip = nullptr;
	// Small text badge at the right (owner, scenario), drawn in badgeColor.
	const char* badge = nullptr;
	ImU32 badgeColor = 0;
	// Shortcut key drawn dim at the right when there is room.
	const char* key = nullptr;
	// The key picks this row as the pilot target, rather than opening it.
	bool pilotKey = true;
	// Only the icon; the name moves to the tooltip.
	bool iconOnly = false;
};
// One full-width rail tab: the selected tab takes the inspector's surface colour
// up to the rail's right edge, so it joins the page it opens. Height 0 uses the
// standard 36 base px row.
bool RailItem(const char* label, Icon icon, bool selected, const RailStatus& status, float height = 0);
} // namespace simxr::app
