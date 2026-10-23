#pragma once
#include "graphics/FrameOutput.h"
#include <array>

namespace simxr::app
{
using SpatialMatrix = std::array<float, 16>;
struct ProjectedPoint
{
	XrVector2f uv{};
	bool behind = false;
};
SpatialMatrix PoseMatrix(XrPosef pose);
SpatialMatrix ViewMatrix(XrPosef camera);
SpatialMatrix ProjectionMatrix(XrFovf fov);
XrPosef MatrixPose(const SpatialMatrix& matrix);
ProjectedPoint ProjectPoint(const FrameView& camera, XrVector3f world);
XrQuaternionf RotationBetween(XrVector3f from, XrVector3f to);
XrQuaternionf ArcballRotation(XrVector2f from, XrVector2f to);
XrQuaternionf SnapRotation(XrQuaternionf rotation, float radians);
// Yaw (about up), pitch (about right) and roll (about forward) in radians; the inverse of EulerAngles.
XrVector3f YawPitchRoll(XrQuaternionf rotation);
XrQuaternionf AxisAngle(XrVector3f axis, float radians);
} // namespace simxr::app
