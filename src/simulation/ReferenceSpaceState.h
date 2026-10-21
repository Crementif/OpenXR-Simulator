#pragma once

#include "simulation/TrackingTypes.h"

#include <openxr/openxr.h>
#include <optional>

namespace simxr
{
struct ReferenceSpaceState
{
	XrPosef local{{0, 0, 0, 1}, {0, 0, 0}};
	XrPosef stage{{0, 0, 0, 1}, {0, 0, 0}};
	std::optional<XrExtent2Df> stageBounds = XrExtent2Df{2, 2};
};

bool NormalizeReferenceSpaces(ReferenceSpaceState& state);
bool RecenterReferenceSpaces(ReferenceSpaceState& state, const PoseSample& head);
bool SameReferenceSpaces(const ReferenceSpaceState& left, const ReferenceSpaceState& right);
} // namespace simxr
