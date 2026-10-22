#pragma once

#include "app/CompanionSettings.h"
#include "app/RuntimeView.h"

namespace simxr::app
{
// Width that fits each row's icon and full name without the shortcut column.
float NarrowRailWidth();
// The device rail rows (icon, name, status dot, owner/scenario badge); keys and
// badges show only when the row has room. Returns true when a row was picked.
bool DrawDeviceRail(Selection& selection, const RuntimeView& view, const CompanionSettings& settings);
} // namespace simxr::app
