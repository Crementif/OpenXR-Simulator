#include "math/Math.h"
#include "Pch.h"


namespace simxr
{
struct Vector3d
{
	double x, y, z;
};

// Preserve intermediate range until the complete transform is narrowed to OpenXR floats.
static Vector3d RotateWide(XrQuaternionf orientation, Vector3d vector)
{
	const double x = orientation.x, y = orientation.y, z = orientation.z, w = orientation.w;
	return {
		(w * w + x * x - y * y - z * z) * vector.x + 2 * (x * y - w * z) * vector.y + 2 * (x * z + w * y) * vector.z,
		2 * (x * y + w * z) * vector.x + (w * w - x * x + y * y - z * z) * vector.y + 2 * (y * z - w * x) * vector.z,
		2 * (x * z - w * y) * vector.x + 2 * (y * z + w * x) * vector.y + (w * w - x * x - y * y + z * z) * vector.z};
}

static XrVector3f Narrow(Vector3d vector)
{
	const auto component = [](double value)
	{
		if (std::abs(value) > std::numeric_limits<float>::max())
			return value < 0 ? -std::numeric_limits<float>::infinity() : std::numeric_limits<float>::infinity();
		return static_cast<float>(value);
	};
	return {component(vector.x), component(vector.y), component(vector.z)};
}

bool IsFinite(XrVector3f vector) noexcept
{
	return std::isfinite(vector.x) && std::isfinite(vector.y) && std::isfinite(vector.z);
}

XrSpaceLocationFlags FinitePoseFlags(XrPosef pose, XrSpaceLocationFlags flags) noexcept
{
	if (!IsFinite(pose.position))
		flags &= ~(XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
	const auto& rotation = pose.orientation;
	if (!std::isfinite(rotation.x) || !std::isfinite(rotation.y) || !std::isfinite(rotation.z) ||
		!std::isfinite(rotation.w))
		flags &= ~(XR_SPACE_LOCATION_ORIENTATION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT);
	return flags;
}

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
	return Narrow(RotateWide(orientation, {vector.x, vector.y, vector.z}));
}

XrQuaternionf EulerAngles(float yaw, float pitch, float roll)
{
	return Multiply(
		Multiply({0, std::sin(yaw / 2), 0, std::cos(yaw / 2)}, {std::sin(pitch / 2), 0, 0, std::cos(pitch / 2)}),
		{0, 0, std::sin(roll / 2), std::cos(roll / 2)});
}

XrPosef Compose(XrPosef parent, XrPosef child)
{
	auto position = RotateWide(parent.orientation, {child.position.x, child.position.y, child.position.z});
	return {Multiply(parent.orientation, child.orientation),
			Narrow({parent.position.x + position.x, parent.position.y + position.y, parent.position.z + position.z})};
}

XrPosef RelativePose(XrPosef target, XrPosef base)
{
	XrQuaternionf inverse{-base.orientation.x, -base.orientation.y, -base.orientation.z, base.orientation.w};
	return {Multiply(inverse, target.orientation),
			Narrow(RotateWide(inverse, {static_cast<double>(target.position.x) - base.position.x,
										static_cast<double>(target.position.y) - base.position.y,
										static_cast<double>(target.position.z) - base.position.z}))};
}
} // namespace simxr
