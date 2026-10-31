#pragma once

#include "simulation/TrackingTypes.h"

#include <string_view>

namespace simxr
{
PoseSample ControllerAimFromGrip(PoseSample grip, std::string_view profile, bool leftHand);
} // namespace simxr
