#include "app/services/SpatialMath.h"
#include "math/Math.h"
#include "Pch.h"

namespace simxr::app
{
SpatialMatrix PoseMatrix(XrPosef pose)
{
	const auto right = Rotate(pose.orientation, {1, 0, 0});
	const auto up = Rotate(pose.orientation, {0, 1, 0});
	const auto back = Rotate(pose.orientation, {0, 0, 1});
	return {right.x,		 right.y,		  right.z,		   0, up.x, up.y, up.z, 0, back.x, back.y, back.z, 0,
			pose.position.x, pose.position.y, pose.position.z, 1};
}
SpatialMatrix ViewMatrix(XrPosef camera)
{
	return PoseMatrix(RelativePose(IdentityPose(), camera));
}
SpatialMatrix ProjectionMatrix(XrFovf fov)
{
	const float left = std::tan(fov.angleLeft), right = std::tan(fov.angleRight);
	const float up = std::tan(fov.angleUp), down = std::tan(fov.angleDown);
	constexpr float near = .01f, far = 1000;
	return {2 / (right - left),
			0,
			0,
			0,
			0,
			2 / (up - down),
			0,
			0,
			(right + left) / (right - left),
			(up + down) / (up - down),
			-(far + near) / (far - near),
			-1,
			0,
			0,
			-2 * far * near / (far - near),
			0};
}
XrPosef MatrixPose(const SpatialMatrix& matrix)
{
	XrQuaternionf rotation{};
	const float trace = matrix[0] + matrix[5] + matrix[10];
	if (trace > 0)
	{
		const float scale = std::sqrt(trace + 1) * 2;
		rotation = {(matrix[6] - matrix[9]) / scale, (matrix[8] - matrix[2]) / scale, (matrix[1] - matrix[4]) / scale,
					.25f * scale};
	}
	else
	{
		const int axis = matrix[0] > matrix[5] && matrix[0] > matrix[10] ? 0 : matrix[5] > matrix[10] ? 1 : 2;
		const int next = (axis + 1) % 3, last = (axis + 2) % 3;
		const float scale = std::sqrt(1 + matrix[axis * 5] - matrix[next * 5] - matrix[last * 5]) * 2;
		std::array<float, 4> components{};
		components[axis] = .25f * scale;
		components[next] = (matrix[axis * 4 + next] + matrix[next * 4 + axis]) / scale;
		components[last] = (matrix[axis * 4 + last] + matrix[last * 4 + axis]) / scale;
		components[3] = (matrix[next * 4 + last] - matrix[last * 4 + next]) / scale;
		rotation = {components[0], components[1], components[2], components[3]};
	}
	return NormalizedPose({rotation, {matrix[12], matrix[13], matrix[14]}}).value_or(IdentityPose());
}
ProjectedPoint ProjectPoint(const FrameView& camera, XrVector3f world)
{
	const auto local = RelativePose({{0, 0, 0, 1}, world}, camera.pose).position;
	const float depth = std::max(.0001f, std::abs(local.z));
	const float left = std::tan(camera.fov.angleLeft), right = std::tan(camera.fov.angleRight);
	const float up = std::tan(camera.fov.angleUp), down = std::tan(camera.fov.angleDown);
	ProjectedPoint result{{(local.x / depth - left) / (right - left), (up - local.y / depth) / (up - down)},
						  local.z >= 0};
	if (result.behind)
		result.uv = {1 - result.uv.x, 1 - result.uv.y};
	return result;
}
XrQuaternionf RotationBetween(XrVector3f from, XrVector3f to)
{
	const auto unit = [](XrVector3f value)
	{
		const float length = std::sqrt(value.x * value.x + value.y * value.y + value.z * value.z);
		return length > .00001f ? XrVector3f{value.x / length, value.y / length, value.z / length}
								: XrVector3f{0, 0, 1};
	};
	from = unit(from);
	to = unit(to);
	float dot = from.x * to.x + from.y * to.y + from.z * to.z;
	if (dot < -.9999f)
	{
		to = std::abs(from.x) < .9f ? XrVector3f{1, 0, 0} : XrVector3f{0, 1, 0};
		dot = -1;
	}
	XrQuaternionf rotation{from.y * to.z - from.z * to.y, from.z * to.x - from.x * to.z, from.x * to.y - from.y * to.x,
						   1 + dot};
	const float length = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z +
								   rotation.w * rotation.w);
	if (length < .00001f)
		return {0, 0, 0, 1};
	return {rotation.x / length, rotation.y / length, rotation.z / length, rotation.w / length};
}
XrQuaternionf ArcballRotation(XrVector2f from, XrVector2f to)
{
	const auto lift = [](XrVector2f point)
	{
		const float squared = point.x * point.x + point.y * point.y;
		if (squared <= 1)
			return XrVector3f{point.x, point.y, std::sqrt(1 - squared)};
		const float length = std::sqrt(squared);
		return XrVector3f{point.x / length, point.y / length, 0};
	};
	const auto first = lift(from), second = lift(to);
	return RotationBetween(first, second);
}

XrQuaternionf SnapRotation(XrQuaternionf rotation, float radians)
{
	if (rotation.w < 0)
		rotation = {-rotation.x, -rotation.y, -rotation.z, -rotation.w};
	const float length = std::sqrt(rotation.x * rotation.x + rotation.y * rotation.y + rotation.z * rotation.z);
	if (length < .00001f)
		return {0, 0, 0, 1};
	const float angle = std::round(2 * std::atan2(length, rotation.w) / radians) * radians;
	const float scale = std::sin(angle / 2) / length;
	return {rotation.x * scale, rotation.y * scale, rotation.z * scale, std::cos(angle / 2)};
}
XrVector3f YawPitchRoll(XrQuaternionf q)
{
	return {std::atan2(2 * (q.x * q.z + q.w * q.y), 1 - 2 * (q.x * q.x + q.y * q.y)),
			std::asin(std::clamp(2 * (q.w * q.x - q.y * q.z), -1.0f, 1.0f)),
			std::atan2(2 * (q.x * q.y + q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z))};
}
XrQuaternionf AxisAngle(XrVector3f axis, float radians)
{
	const float length = std::sqrt(axis.x * axis.x + axis.y * axis.y + axis.z * axis.z);
	if (length < .00001f)
		return {0, 0, 0, 1};
	const float scale = std::sin(radians / 2) / length;
	return {axis.x * scale, axis.y * scale, axis.z * scale, std::cos(radians / 2)};
}
} // namespace simxr::app
