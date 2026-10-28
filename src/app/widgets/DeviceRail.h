#pragma once

#include "app/RuntimeView.h"
#include "app/services/CompanionSettings.h"

#include <optional>

namespace simxr::app
{
float RailWidth();
float DeviceTabsHeight();
std::optional<Selection> DrawDeviceRail(const Selection& selection, const RuntimeView& view,
										const CompanionSettings& settings);
std::optional<Selection> DrawDeviceTabs(const Selection& selection, const RuntimeView& view,
										const CompanionSettings& settings);
} // namespace simxr::app
