#include "graphics/PreviewGeometry.h"

#include "Math.h"

#include <algorithm>
#include <cmath>
#include <numbers>

namespace simxr
{
namespace
{
std::optional<XrPosef> PreviewPose(const PoseSample& sample)
{
	if ((sample.flags & ValidPoseFlags) != ValidPoseFlags)
		return {};
	return NormalizedPose(sample.pose);
}

std::optional<std::array<float, 4>> Tangents(const XrFovf& fov)
{
	const std::array angles{fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
	std::array<float, 4> tangents{};
	for (std::size_t index = 0; index < angles.size(); ++index)
	{
		if (!std::isfinite(angles[index]) || std::abs(angles[index]) >= std::numbers::pi_v<float> / 2)
			return {};
		tangents[index] = std::tan(angles[index]);
	}
	if (tangents[1] - tangents[0] <= 0.000001f || tangents[2] - tangents[3] <= 0.000001f)
		return {};
	return tangents;
}

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

float PlaneDistance(const ClipPoint& point, std::size_t plane)
{
	switch (plane)
	{
	case 0:
		return point[3];
	case 1:
		return point[3] + point[0];
	case 2:
		return point[3] - point[0];
	case 3:
		return point[3] + point[1];
	default:
		return point[3] - point[1];
	}
}

ClippedPolygon ClipPolygon(const ClippedPolygon& input, std::size_t plane)
{
	ClippedPolygon output;
	if (!input.count)
		return output;
	auto previous = input.points[input.count - 1];
	float previousDistance = PlaneDistance(previous, plane);
	for (std::size_t index = 0; index < input.count; ++index)
	{
		const auto& current = input.points[index];
		const float currentDistance = PlaneDistance(current, plane);
		if ((previousDistance >= 0) != (currentDistance >= 0))
		{
			const float fraction = previousDistance / (previousDistance - currentDistance);
			ClipPoint intersection;
			for (std::size_t component = 0; component < intersection.size(); ++component)
				intersection[component] = previous[component] + fraction * (current[component] - previous[component]);
			output.points.at(output.count++) = intersection;
		}
		if (currentDistance >= 0)
			output.points.at(output.count++) = current;
		previous = current;
		previousDistance = currentDistance;
	}
	return output;
}
} // namespace

PreviewEyeLayout PreparePreviewEyeLayout(PreviewRectangle present, PreviewEyeSelection selection,
										 PreviewEyePacking packing, PreviewPixelAlignment alignment)
{
	PreviewEyeLayout result;
	if (!std::isfinite(present.x) || !std::isfinite(present.y) || !std::isfinite(present.width) ||
		!std::isfinite(present.height) || present.width <= 0 || present.height <= 0)
		return result;
	if (selection != PreviewEyeSelection::Both)
	{
		result.eyes[0] = {selection == PreviewEyeSelection::Right ? 1u : 0u, present};
		result.count = 1;
		return result;
	}
	result.eyes = {{{0, present}, {1, present}}};
	result.count = 2;
	if (packing == PreviewEyePacking::SideBySide)
	{
		float width = present.width * 0.5f;
		if (alignment == PreviewPixelAlignment::WholePixels)
			width = std::floor(width);
		result.eyes[0].rectangle.width = width;
		result.eyes[1].rectangle.width = width;
		result.eyes[1].rectangle.x += result.eyes[0].rectangle.width;
	}
	else if (packing == PreviewEyePacking::OverUnder)
	{
		float height = present.height * 0.5f;
		if (alignment == PreviewPixelAlignment::WholePixels)
			height = std::floor(height);
		result.eyes[0].rectangle.height = height;
		result.eyes[1].rectangle.height = height;
		result.eyes[1].rectangle.y += result.eyes[0].rectangle.height;
	}
	return result;
}

std::optional<PreviewProjection> PreparePreviewProjection(const ProjectionLayerDescription& layer,
														  const CompositionExtentResolver& resolve,
														  PreviewCropSelection crop)
{
	if (!resolve || (layer.views.size() != 2 && layer.views.size() != 4))
		return {};
	PreviewProjection projection;
	for (std::uint32_t eye = 0; eye < projection.eyes.size(); ++eye)
	{
		const auto& accepted = layer.views[eye].color;
		if (accepted.source == XR_NULL_HANDLE)
			return {};
		const auto extent = resolve(accepted.source);
		if (!extent || extent->width <= 0 || extent->height <= 0)
			return {};
		auto image = accepted;
		if (crop == PreviewCropSelection::WholeImage)
			image.rectangle = {{0, 0}, *extent};
		const auto& rectangle = image.rectangle;
		if (rectangle.offset.x < 0 || rectangle.offset.y < 0 || rectangle.extent.width < 0 ||
			rectangle.extent.height < 0 ||
			static_cast<std::int64_t>(rectangle.offset.x) + rectangle.extent.width > extent->width ||
			static_cast<std::int64_t>(rectangle.offset.y) + rectangle.extent.height > extent->height)
			return {};
		if (!rectangle.extent.width || !rectangle.extent.height)
			continue;
		PreviewProjectionSource source{eye,
									   image,
									   *extent,
									   {static_cast<float>(rectangle.offset.x) / extent->width,
										static_cast<float>(rectangle.offset.y) / extent->height},
									   {static_cast<float>(rectangle.extent.width) / extent->width,
										static_cast<float>(rectangle.extent.height) / extent->height}};
		projection.displayExtent.width = std::max(projection.displayExtent.width, rectangle.extent.width);
		projection.displayExtent.height = std::max(projection.displayExtent.height, rectangle.extent.height);
		projection.eyes[eye] = source;
	}
	return projection;
}

PreviewProjectionDraws PreparePreviewProjectionDraws(const PreviewProjection& source, const PreviewEyeLayout& layout,
													 PreviewEyePacking packing)
{
	PreviewProjectionDraws result;
	for (std::size_t index = 0; index < std::min(layout.count, layout.eyes.size()); ++index)
	{
		const auto& target = layout.eyes[index];
		if (target.eye >= source.eyes.size() || !source.eyes[target.eye] || target.rectangle.width <= 0 ||
			target.rectangle.height <= 0)
			continue;
		auto channels = PreviewColorChannels::All;
		if (layout.count == 2 && packing == PreviewEyePacking::Anaglyph)
			channels = target.eye == 0 ? PreviewColorChannels::Red : PreviewColorChannels::Cyan;
		result.draws[result.count++] = {target.eye, target, channels};
	}
	return result;
}

std::optional<std::array<float, 8>> PreviewProjectionConstants(const PreviewProjectionSource& source,
															   PreviewRectangle viewport, bool encodeSrgb)
{
	if (!std::isfinite(viewport.width) || !std::isfinite(viewport.height) || viewport.width <= 0 ||
		viewport.height <= 0)
		return {};
	const std::array<float, 8> constants{source.uvOffset[0],
										 source.uvOffset[1],
										 source.uvScale[0],
										 source.uvScale[1],
										 0.25f * source.uvScale[0] / viewport.width,
										 0.25f * source.uvScale[1] / viewport.height,
										 encodeSrgb ? 1.0f : 0.0f,
										 0};
	if (!std::all_of(constants.begin(), constants.end(),
					 [](float value)
					 {
						 return std::isfinite(value);
					 }))
		return {};
	return constants;
}

std::optional<PreviewQuadGeometry> PreparePreviewQuad(const QuadLayerDescription& quad, const LocatedViews& views,
													  XrExtent2Di sourceExtent)
{
	const auto worldPose = PreviewPose(quad.worldPose);
	const auto& rectangle = quad.color.rectangle;
	if (quad.color.source == XR_NULL_HANDLE || !worldPose || sourceExtent.width <= 0 || sourceExtent.height <= 0 ||
		rectangle.offset.x < 0 || rectangle.offset.y < 0 || rectangle.extent.width <= 0 ||
		rectangle.extent.height <= 0 ||
		static_cast<std::int64_t>(rectangle.offset.x) + rectangle.extent.width > sourceExtent.width ||
		static_cast<std::int64_t>(rectangle.offset.y) + rectangle.extent.height > sourceExtent.height ||
		!std::isfinite(quad.size.width) || !std::isfinite(quad.size.height) || quad.size.width <= 0 ||
		quad.size.height <= 0 ||
		(quad.eyes != XR_EYE_VISIBILITY_BOTH && quad.eyes != XR_EYE_VISIBILITY_LEFT &&
		 quad.eyes != XR_EYE_VISIBILITY_RIGHT))
		return {};
	PreviewQuadGeometry geometry;
	geometry.source = quad.color;
	geometry.uvRectangle = {static_cast<float>(rectangle.offset.x) / sourceExtent.width,
							static_cast<float>(rectangle.offset.y) / sourceExtent.height,
							static_cast<float>(rectangle.offset.x + rectangle.extent.width) / sourceExtent.width,
							static_cast<float>(rectangle.offset.y + rectangle.extent.height) / sourceExtent.height};
	const float halfWidth = quad.size.width / 2;
	const float halfHeight = quad.size.height / 2;
	const std::array<XrVector3f, 4> localCorners{{{-halfWidth, halfHeight, 0},
												  {halfWidth, halfHeight, 0},
												  {halfWidth, -halfHeight, 0},
												  {-halfWidth, -halfHeight, 0}}};
	for (const auto& view : views.views)
	{
		const auto eye = view.description.eye;
		if (view.description.role != ViewRole::Outer || eye >= geometry.eyes.size() ||
			(quad.eyes == XR_EYE_VISIBILITY_LEFT && eye != 0) || (quad.eyes == XR_EYE_VISIBILITY_RIGHT && eye != 1))
			continue;
		const auto viewPose = PreviewPose(view.pose);
		const auto tangents = Tangents(view.fov);
		if (!viewPose || !tangents)
			continue;
		PreviewQuadEye projected;
		projected.tangents = *tangents;
		const auto quadInView = RelativePose(*worldPose, *viewPose);
		for (std::size_t corner = 0; corner < localCorners.size(); ++corner)
		{
			const auto rotated = Rotate(quadInView.orientation, localCorners[corner]);
			projected.viewCorners[corner] = {quadInView.position.x + rotated.x, quadInView.position.y + rotated.y,
											 quadInView.position.z + rotated.z};
		}
		constexpr std::array<std::size_t, 4> StripOrder{0, 1, 3, 2};
		bool finite = true;
		for (std::size_t corner = 0; corner < StripOrder.size(); ++corner)
		{
			projected.stripClipCorners[corner] = ClipCorner(projected.viewCorners[StripOrder[corner]], *tangents);
			for (float component : projected.stripClipCorners[corner])
				finite = finite && std::isfinite(component);
		}
		if (finite)
			geometry.eyes[eye] = projected;
	}
	return geometry;
}

std::array<float, 28> PreviewQuadConstants(const PreviewQuadGeometry& geometry, std::uint32_t eye, bool opaque)
{
	std::array<float, 28> constants{};
	if (eye >= geometry.eyes.size() || !geometry.eyes[eye])
		return constants;
	const auto& projected = *geometry.eyes[eye];
	for (std::size_t corner = 0; corner < projected.viewCorners.size(); ++corner)
	{
		constants[corner * 4] = projected.viewCorners[corner].x;
		constants[corner * 4 + 1] = projected.viewCorners[corner].y;
		constants[corner * 4 + 2] = projected.viewCorners[corner].z;
	}
	std::copy(projected.tangents.begin(), projected.tangents.end(), constants.begin() + 16);
	std::copy(geometry.uvRectangle.begin(), geometry.uvRectangle.end(), constants.begin() + 20);
	constants[24] = opaque ? 1.0f : 0.0f;
	return constants;
}

std::optional<XrRect2Di> PreviewQuadBounds(const PreviewQuadEye& geometry, PreviewRectangle viewport,
										   XrExtent2Di targetExtent)
{
	if (targetExtent.width <= 0 || targetExtent.height <= 0 || !std::isfinite(viewport.x) ||
		!std::isfinite(viewport.y) || !std::isfinite(viewport.width) || !std::isfinite(viewport.height) ||
		viewport.width <= 0 || viewport.height <= 0)
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
		const double x = viewport.x + (static_cast<double>(point[0]) / point[3] + 1) * 0.5 * viewport.width;
		const double y = viewport.y + (1 - static_cast<double>(point[1]) / point[3]) * 0.5 * viewport.height;
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
		PreviewQuadEye geometry;
		geometry.tangents = constants.eyeTangents;
		const std::array<XrVector3f, 4> corners{{{-halfWidth, halfHeight, 0}, {halfWidth, halfHeight, 0},
			{halfWidth, -halfHeight, 0}, {-halfWidth, -halfHeight, 0}}};
		for (std::size_t index = 0; index < corners.size(); ++index)
			geometry.viewCorners[index] = Compose(eyeFromQuad, {{0, 0, 0, 1}, corners[index]}).position;
		const auto bounds = PreviewQuadBounds(geometry, {0, 0, static_cast<float>(plan.width), static_cast<float>(plan.height)},
			{static_cast<std::int32_t>(plan.width), static_cast<std::int32_t>(plan.height)});
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
