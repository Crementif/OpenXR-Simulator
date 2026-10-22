#pragma once

#include "app/CompanionSettings.h"
#include "app/RuntimeView.h"

#include <optional>

namespace simxr::app
{
// Width that fits each row's icon and full name without the shortcut column.
float NarrowRailWidth();
// Width of the icon-only rail used below 800 effective pixels; names go to tooltips.
float IconRailWidth();
// The rail's tabs (icon, name, status dot, owner/scenario badge) under DEVICES
// and SETUP; keys and badges show only when the row has room. Returns the
// clicked row's selection, including the selected row clicked again. reserve
// keeps that much height free below the tabs.
std::optional<Selection> DrawDeviceRail(const Selection& selection, const RuntimeView& view,
										const CompanionSettings& settings, bool iconOnly = false,
										float reserve = 0);
} // namespace simxr::app
