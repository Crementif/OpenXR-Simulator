#pragma once

#include "simulation/TrackingTypes.h"

#include <string_view>

namespace simxr
{
PoseSample ControllerAimFromGrip(PoseSample grip, std::string_view profile, bool leftHand);
// The grip pose whose derived aim is `aim`; the inverse of ControllerAimFromGrip.
XrPosef ControllerGripFromAim(XrPosef aim, std::string_view profile, bool leftHand);
// Menus usually float a few metres out; the hand's offset from the eye makes the miss grow with the gap.
inline constexpr float DefaultPointDistance = 3;
// The point `distance` metres along the ray through `uv` (0..1 from the image's top left) of a view.
XrVector3f ViewRayPoint(XrPosef eye, XrFovf fov, XrVector2f uv, float distance);
// An orientation looking from `origin` towards `target` with no roll.
XrQuaternionf LookAt(XrVector3f origin, XrVector3f target);
} // namespace simxr
