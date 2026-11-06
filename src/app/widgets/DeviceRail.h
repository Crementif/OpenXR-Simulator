#pragma once

#include "app/RuntimeView.h"
#include "app/services/CompanionSettings.h"

#include <optional>

namespace simxr::app
{
// The device page whose key was pressed this frame.
std::optional<DevicePage> DeviceShortcut(const CompanionSettings& settings);
float RailWidth(const CompanionSettings& settings);
float DeviceTabsHeight();
std::optional<Selection> DrawDeviceRail(const Selection& selection, const RuntimeView& view,
										const CompanionSettings& settings);
std::optional<Selection> DrawDeviceTabs(const Selection& selection, const RuntimeView& view,
										const CompanionSettings& settings);
} // namespace simxr::app
