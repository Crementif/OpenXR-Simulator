#pragma once

#include "simulation/SpaceSystem.h"
#include "runtime/ExtensionRegistry.h"
#include "simulation/ViewConfiguration.h"

#include <openxr/openxr.h>

namespace simxr
{
XrResult LocateSpace(SpaceSystem& spaces, XrSpace space, XrSpace base, XrTime time, XrSpaceLocation* location,
	const EnabledExtensions* enabled = nullptr);
XrResult LocateViews(SpaceSystem& spaces, const ViewConfiguration& configuration, const TrackingSnapshot& snapshot,
	const XrViewLocateInfo& info, XrViewState* state, std::uint32_t capacity, std::uint32_t* count, XrView* views,
	const EnabledExtensions* enabled = nullptr, ViewQuerySnapshot* query = nullptr);
}
