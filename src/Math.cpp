#include "Math.h"

#include <cmath>

namespace simxr
{
XrPosef IdentityPose()
{
	return {{0, 0, 0, 1}, {0, 0, 0}};
}

std::optional<XrPosef> NormalizedPose(XrPosef pose) noexcept
{
	auto& rotation = pose.orientation;
	const float length = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z +
								   rotation.w * rotation.w);
	if (!std::isfinite(length) || std::abs(length - 1) > 0.01f || !std::isfinite(pose.position.x) ||
		!std::isfinite(pose.position.y) || !std::isfinite(pose.position.z))
		return std::nullopt;
	rotation = {rotation.x / length, rotation.y / length, rotation.z / length, rotation.w / length};
	return pose;
}

XrQuaternionf Multiply(XrQuaternionf left, XrQuaternionf right)
{
	return {left.w * right.x + left.x * right.w + left.y * right.z - left.z * right.y,
			left.w * right.y - left.x * right.z + left.y * right.w + left.z * right.x,
			left.w * right.z + left.x * right.y - left.y * right.x + left.z * right.w,
			left.w * right.w - left.x * right.x - left.y * right.y - left.z * right.z};
}

XrVector3f Rotate(XrQuaternionf orientation, XrVector3f vector)
{
	auto rotated = Multiply(Multiply(orientation, {vector.x, vector.y, vector.z, 0}),
							{-orientation.x, -orientation.y, -orientation.z, orientation.w});
	return {rotated.x, rotated.y, rotated.z};
}

XrQuaternionf EulerAngles(float yaw, float pitch, float roll)
{
	return Multiply(
		Multiply({0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)}),
		{0, 0, std::sin(roll / 2), std::cos(roll / 2)});
}

XrPosef Compose(XrPosef parent, XrPosef child)
{
	auto position = Rotate(parent.orientation, child.position);
	return {Multiply(parent.orientation, child.orientation),
			{parent.position.x + position.x, parent.position.y + position.y, parent.position.z + position.z}};
}

XrPosef RelativePose(XrPosef target, XrPosef base)
{
	XrQuaternionf inverse{-base.orientation.x, -base.orientation.y, -base.orientation.z, base.orientation.w};
	return {Multiply(inverse, target.orientation),
			Rotate(inverse, {target.position.x - base.position.x, target.position.y - base.position.y,
							 target.position.z - base.position.z})};
}
} // namespace simxr
