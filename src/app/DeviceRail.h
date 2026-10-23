#pragma once

#include "app/CompanionSettings.h"
#include "app/RuntimeView.h"

#include <optional>

namespace simxr::app
{
// Width of the icon rail shown while the inspector is folded or in a narrow window.
float RailWidth();
float DeviceTabsHeight();
// The seven device tabs as an icon column; names, status and pilot keys go to tooltips.
// Returns the clicked tab's selection, including the selected tab clicked again.
std::optional<Selection> DrawDeviceRail(const Selection& selection, const RuntimeView& view,
										const CompanionSettings& settings);
// The same tabs across the top of the inspector: icons with status dots, and the
// selected tab's name beside its icon when the strip has room.
std::optional<Selection> DrawDeviceTabs(const Selection& selection, const RuntimeView& view,
										const CompanionSettings& settings);
} // namespace simxr::app
