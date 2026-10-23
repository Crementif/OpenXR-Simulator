#include "graphics/Compositor.h"
#include "Math.h"

#include <algorithm>
#include <cmath>

namespace simxr
{
namespace
{
struct QuadGeometry
{
	std::array<XrVector3f, 4> viewCorners;
	std::array<float, 4> tangents;
};

std::array<float, 4> ClipCorner(XrVector3f point, const std::array<float, 4>& tangents)
{
	const float horizontal = tangents[1] - tangents[0];
	const float vertical = tangents[2] - tangents[3];
	return {(2 * point.x + (tangents[1] + tangents[0]) * point.z) / horizontal,
			(2 * point.y + (tangents[2] + tangents[3]) * point.z) / vertical, -0.5f * point.z, -point.z};
}

using ClipPoint = std::array<float, 4>;
struct ClippedPolygon
{
	std::array<ClipPoint, 16> points{};
	std::size_t count = 0;
};

double PlaneDistance(const ClipPoint& point, std::size_t plane)
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

ClippedPolygon ClipPolygon(const ClippedPolygon& input, std::size_t plane)
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
				intersection[component] = static_cast<float>(std::lerp(
					static_cast<double>(previous[component]), static_cast<double>(current[component]), fraction));
			output.points.at(output.count++) = intersection;
		}
		if (currentDistance >= 0)
			output.points.at(output.count++) = current;
		previous = current;
		previousDistance = currentDistance;
	}
	return output;
}
std::optional<XrRect2Di> QuadBounds(const QuadGeometry& geometry, XrExtent2Di targetExtent)
{
	if (targetExtent.width <= 0 || targetExtent.height <= 0)
		return {};
	ClippedPolygon polygon;
	for (const auto& corner : geometry.viewCorners)
	{
		const auto clip = ClipCorner(corner, geometry.tangents);
		for (float component : clip)
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
		const double x = (static_cast<double>(point[0]) / point[3] + 1) * 0.5 * targetExtent.width;
		const double y = (1 - static_cast<double>(point[1]) / point[3]) * 0.5 * targetExtent.height;
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
} // namespace

std::array<XrRect2Di, 2> CompositionQuadBounds(const CompositionPlan& plan)
{
	std::array<XrRect2Di, 2> result{};
	for (const auto& draw : plan.draws)
	{
		if (draw.constants.control[0] != 1 || draw.eye >= result.size())
			continue;
		const auto& constants = draw.constants;
		const auto& rotation = constants.sourceFromEyeRotation;
		const auto& position = constants.sourceFromEyePosition;
		const auto eyeFromQuad = RelativePose(IdentityPose(),
			{{rotation[0], rotation[1], rotation[2], rotation[3]}, {position[0], position[1], position[2]}});
		const float halfWidth = 0.5f / constants.quadParameters[0];
		const float halfHeight = 0.5f / constants.quadParameters[1];
		QuadGeometry geometry;
		geometry.tangents = constants.eyeTangents;
		const std::array<XrVector3f, 4> corners{{{-halfWidth, halfHeight, 0}, {halfWidth, halfHeight, 0},
			{halfWidth, -halfHeight, 0}, {-halfWidth, -halfHeight, 0}}};
		for (std::size_t index = 0; index < corners.size(); ++index)
			geometry.viewCorners[index] = Compose(eyeFromQuad, {{0, 0, 0, 1}, corners[index]}).position;
		const auto bounds = QuadBounds(geometry, {static_cast<std::int32_t>(plan.width), static_cast<std::int32_t>(plan.height)});
		if (!bounds)
			continue;
		auto& combined = result[draw.eye];
		if (!combined.extent.width || !combined.extent.height)
			combined = *bounds;
		else
		{
			const auto left = std::min(combined.offset.x, bounds->offset.x);
			const auto top = std::min(combined.offset.y, bounds->offset.y);
			const auto right = std::max(combined.offset.x + combined.extent.width, bounds->offset.x + bounds->extent.width);
			const auto bottom = std::max(combined.offset.y + combined.extent.height, bounds->offset.y + bounds->extent.height);
			combined = {{left, top}, {right - left, bottom - top}};
		}
	}
	return result;
}
} // namespace simxr
