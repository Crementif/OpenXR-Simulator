#pragma once

#include <openxr/openxr.h>
#include <optional>

namespace simxr
{
bool IsFinite(XrVector3f vector) noexcept;
XrSpaceLocationFlags FinitePoseFlags(XrPosef pose, XrSpaceLocationFlags flags) noexcept;
XrPosef IdentityPose();
std::optional<XrPosef> NormalizedPose(XrPosef pose) noexcept;
XrQuaternionf Multiply(XrQuaternionf left, XrQuaternionf right);
XrVector3f Rotate(XrQuaternionf orientation, XrVector3f vector);
XrQuaternionf EulerAngles(float yaw, float pitch, float roll);
XrPosef Compose(XrPosef parent, XrPosef child);
XrPosef RelativePose(XrPosef target, XrPosef base);
} // namespace simxr
