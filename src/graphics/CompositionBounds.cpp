#include "graphics/Compositor.h"
#include "Pch.h"
#include "Math.h"

#include <cmath>
#include <limits>

namespace simxr
{
struct QuadGeometry
{
	std::array<std::array<double, 3>, 4> viewCorners;
	std::array<float, 4> tangents;
};

using ClipPoint = std::array<double, 4>;

static ClipPoint ClipCorner(const std::array<double, 3>& point, const std::array<float, 4>& tangents)
{
	const double horizontal = static_cast<double>(tangents[1]) - tangents[0];
	const double vertical = static_cast<double>(tangents[2]) - tangents[3];
	const double x = point[0], y = point[1], z = point[2];
	return {(2 * x + (static_cast<double>(tangents[1]) + tangents[0]) * z) / horizontal,
			(2 * y + (static_cast<double>(tangents[2]) + tangents[3]) * z) / vertical, -0.5 * z, -z};
}

struct ClippedPolygon
{
	std::array<ClipPoint, 16> points{};
	std::size_t count = 0;
};

static double PlaneDistance(const ClipPoint& point, std::size_t plane)
{
	const double w = point[3];
	switch (plane)
	{
	case 0:
		return w;
	case 1:
		return w + point[0];
	case 2:
		return w - point[0];
	case 3:
		return w + point[1];
	default:
		return w - point[1];
	}
}

static ClippedPolygon ClipPolygon(const ClippedPolygon& input, std::size_t plane)
{
	ClippedPolygon output;
	if (!input.count)
		return output;
	auto previous = input.points[input.count - 1];
	double previousDistance = PlaneDistance(previous, plane);
	for (std::size_t index = 0; index < input.count; ++index)
	{
		const auto& current = input.points[index];
		const double currentDistance = PlaneDistance(current, plane);
		if ((previousDistance >= 0) != (currentDistance >= 0))
		{
			const double fraction = previousDistance / (previousDistance - currentDistance);
			ClipPoint intersection;
			for (std::size_t component = 0; component < intersection.size(); ++component)
				intersection[component] = std::lerp(previous[component], current[component], fraction);
			output.points.at(output.count++) = intersection;
		}
		if (currentDistance >= 0)
			output.points.at(output.count++) = current;
		previous = current;
		previousDistance = currentDistance;
	}
	return output;
}

static std::optional<XrRect2Di> QuadBounds(const QuadGeometry& geometry, XrExtent2Di targetExtent)
{
	if (targetExtent.width <= 0 || targetExtent.height <= 0)
		return {};
	ClippedPolygon polygon;
	for (const auto& corner : geometry.viewCorners)
	{
		const auto clip = ClipCorner(corner, geometry.tangents);
		for (double component : clip)
			if (!std::isfinite(component))
				return {};
		polygon.points[polygon.count++] = clip;
	}
	for (std::size_t plane = 0; plane < 5; ++plane)
		polygon = ClipPolygon(polygon, plane);
	if (!polygon.count)
		return {};
	double minimumX = targetExtent.width;
	double minimumY = targetExtent.height;
	double maximumX = 0;
	double maximumY = 0;
	for (std::size_t index = 0; index < polygon.count; ++index)
	{
		const auto& point = polygon.points[index];
		if (point[3] <= 0)
			continue;
		const double x = (point[0] / point[3] + 1) * 0.5 * targetExtent.width;
		const double y = (1 - point[1] / point[3]) * 0.5 * targetExtent.height;
		minimumX = std::min(minimumX, x);
		minimumY = std::min(minimumY, y);
		maximumX = std::max(maximumX, x);
		maximumY = std::max(maximumY, y);
	}
	const auto left =
		static_cast<std::int32_t>(std::floor(std::clamp(minimumX, 0.0, static_cast<double>(targetExtent.width))));
	const auto top =
		static_cast<std::int32_t>(std::floor(std::clamp(minimumY, 0.0, static_cast<double>(targetExtent.height))));
	const auto right =
		static_cast<std::int32_t>(std::ceil(std::clamp(maximumX, 0.0, static_cast<double>(targetExtent.width))));
	const auto bottom =
		static_cast<std::int32_t>(std::ceil(std::clamp(maximumY, 0.0, static_cast<double>(targetExtent.height))));
	if (right <= left || bottom <= top)
		return {};
	return XrRect2Di{{left, top}, {right - left, bottom - top}};
}

std::array<XrRect2Di, 2> CompositionQuadBounds(const CompositionPlan& plan)
{
	std::array<XrRect2Di, 2> result{};
	if (plan.width > std::numeric_limits<std::int32_t>::max() || plan.height > std::numeric_limits<std::int32_t>::max())
		return result;
	const XrExtent2Di targetExtent{static_cast<std::int32_t>(plan.width), static_cast<std::int32_t>(plan.height)};
	for (const auto& draw : plan.draws)
	{
		if (draw.constants.control[0] != 1 || draw.eye >= result.size())
			continue;
		const auto& constants = draw.constants;
		const auto& rotation = constants.sourceFromEyeRotation;
		const auto& position = constants.sourceFromEyePosition;
		if (position[2] <= 0)
			continue;
		const XrQuaternionf inverse{-rotation[0], -rotation[1], -rotation[2], rotation[3]};
		const std::array axes{Rotate(inverse, {1, 0, 0}), Rotate(inverse, {0, 1, 0}), Rotate(inverse, {0, 0, 1})};
		const float halfWidth = 0.5f / constants.quadParameters[0];
		const float halfHeight = 0.5f / constants.quadParameters[1];
		QuadGeometry geometry;
		geometry.tangents = constants.eyeTangents;
		const std::array<XrVector3f, 4> corners{{{-halfWidth, halfHeight, 0},
												 {halfWidth, halfHeight, 0},
												 {halfWidth, -halfHeight, 0},
												 {-halfWidth, -halfHeight, 0}}};
		for (std::size_t index = 0; index < corners.size(); ++index)
		{
			const double x = static_cast<double>(corners[index].x) - position[0];
			const double y = static_cast<double>(corners[index].y) - position[1];
			const double z = static_cast<double>(corners[index].z) - position[2];
			geometry.viewCorners[index] = {axes[0].x * x + axes[1].x * y + axes[2].x * z,
										   axes[0].y * x + axes[1].y * y + axes[2].y * z,
										   axes[0].z * x + axes[1].z * y + axes[2].z * z};
		}
		const auto bounds = QuadBounds(geometry, targetExtent);
		if (!bounds)
			continue;
		auto& combined = result[draw.eye];
		if (!combined.extent.width || !combined.extent.height)
			combined = *bounds;
		else
		{
			const auto left = std::min(combined.offset.x, bounds->offset.x);
			const auto top = std::min(combined.offset.y, bounds->offset.y);
			const auto right =
				std::max(combined.offset.x + combined.extent.width, bounds->offset.x + bounds->extent.width);
			const auto bottom =
				std::max(combined.offset.y + combined.extent.height, bounds->offset.y + bounds->extent.height);
			combined = {{left, top}, {right - left, bottom - top}};
		}
	}
	return result;
}
} // namespace simxr
