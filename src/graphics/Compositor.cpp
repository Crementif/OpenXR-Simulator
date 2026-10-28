#include "graphics/Compositor.h"
#include "Pch.h"

#include "math/Math.h"


namespace simxr
{
static bool FovTangents(const XrFovf& fov, std::array<float, 4>& output)
{
	const std::array angles{fov.angleLeft, fov.angleRight, fov.angleUp, fov.angleDown};
	for (std::size_t index = 0; index < angles.size(); ++index)
	{
		if (!std::isfinite(angles[index]) || std::abs(angles[index]) >= std::numbers::pi_v<float> / 2)
			return false;
		output[index] = std::tan(angles[index]);
	}
	return output[0] != output[1] && output[2] != output[3];
}
static bool ValidLayerFlags(XrCompositionLayerFlags flags)
{
	constexpr auto known = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT |
						   XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT |
						   XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	return !(flags & ~known);
}
static XrResult ImageConstants(const LayerImageDescription& image, const CompositionExtentResolver& resolve,
							   LayerConstants& constants)
{
	if (!resolve || image.source == XR_NULL_HANDLE)
		return XR_ERROR_HANDLE_INVALID;
	const auto extent = resolve(image.source);
	if (!extent)
		return XR_ERROR_HANDLE_INVALID;
	const auto& rect = image.rectangle;
	if (extent->width <= 0 || extent->height <= 0 || rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width < 0 ||
		rect.extent.height < 0 || static_cast<std::int64_t>(rect.offset.x) + rect.extent.width > extent->width ||
		static_cast<std::int64_t>(rect.offset.y) + rect.extent.height > extent->height)
		return XR_ERROR_SWAPCHAIN_RECT_INVALID;
	constants.imageRectangle = {
		static_cast<float>(rect.offset.x) / extent->width, static_cast<float>(rect.offset.y) / extent->height,
		static_cast<float>(rect.extent.width) / extent->width, static_cast<float>(rect.extent.height) / extent->height};
	return XR_SUCCESS;
}
static bool SourceFromEye(const PoseSample& source, const PoseSample& eye, LayerConstants& constants)
{
	const auto eyePose = NormalizedPose(eye.pose);
	const auto sourcePose = NormalizedPose(source.pose);
	if (!eyePose || !sourcePose)
		return false;
	const auto relative = RelativePose(*eyePose, *sourcePose);
	if (!NormalizedPose(relative))
		return false;
	const auto rotation = relative.orientation;
	constants.sourceFromEyeRotation = {rotation.x, rotation.y, rotation.z, rotation.w};
	constants.sourceFromEyePosition = {relative.position.x, relative.position.y, relative.position.z, 0};
	return true;
}
static std::uint32_t AlphaFlags(XrCompositionLayerFlags flags)
{
	return ((flags & XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT) ? 1u : 0u) |
		   ((flags & XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT) ? 2u : 0u);
}
static bool EmptyImage(const LayerImageDescription& image)
{
	return image.rectangle.extent.width == 0 || image.rectangle.extent.height == 0;
}

XrResult PrepareDepthComposition(const LayerDepthDescription& depth, bool linear, XrExtent2Di extent, bool multisampled,
								 CompositionPlan& output)
{
	if (linear && (!std::isfinite(depth.nearZ) || !std::isfinite(depth.farZ)))
		return XR_ERROR_FEATURE_UNSUPPORTED;
	if (extent.width <= 0 || extent.height <= 0)
		return XR_ERROR_SWAPCHAIN_RECT_INVALID;
	const auto& rectangle = depth.image.rectangle;
	if (rectangle.offset.x < 0 || rectangle.offset.y < 0 || rectangle.extent.width <= 0 ||
		rectangle.extent.height <= 0 ||
		static_cast<std::uint64_t>(rectangle.offset.x) + rectangle.extent.width >
			static_cast<std::uint32_t>(extent.width) ||
		static_cast<std::uint64_t>(rectangle.offset.y) + rectangle.extent.height >
			static_cast<std::uint32_t>(extent.height))
		return XR_ERROR_SWAPCHAIN_RECT_INVALID;
	if (!std::isfinite(depth.minDepth) || !std::isfinite(depth.maxDepth) || depth.minDepth < 0 || depth.maxDepth > 1 ||
		depth.minDepth >= depth.maxDepth || depth.nearZ <= 0 || depth.farZ <= 0 || depth.nearZ == depth.farZ ||
		std::isnan(depth.nearZ) || std::isnan(depth.farZ))
		return XR_ERROR_VALIDATION_FAILURE;
	CompositionPlan plan;
	plan.width = static_cast<std::uint32_t>(rectangle.extent.width);
	plan.height = static_cast<std::uint32_t>(rectangle.extent.height);
	CompositionDraw draw{depth.image, 0, {}};
	draw.constants.imageRectangle = {static_cast<float>(rectangle.offset.x), static_cast<float>(rectangle.offset.y),
									 static_cast<float>(rectangle.extent.width),
									 static_cast<float>(rectangle.extent.height)};
	draw.constants.quadParameters = {depth.minDepth, depth.maxDepth, 0, 0};
	if (linear)
	{
		const auto scale = std::max(depth.nearZ, depth.farZ);
		draw.constants.quadParameters[2] = depth.nearZ / scale;
		draw.constants.quadParameters[3] = depth.farZ / scale;
	}
	draw.constants.control = {2, linear ? 1U : 0U, multisampled ? 1U : 0U, 0};
	plan.draws.push_back(draw);
	output = std::move(plan);
	return XR_SUCCESS;
}

XrResult PrepareComposition(const FrameLayers& frame, const CompositionTarget& target,
							const CompositionExtentResolver& resolve, CompositionPlan& output)
{
	constexpr auto maxExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
	if (!target.width || !target.height || target.width > maxExtent || target.height > maxExtent ||
		frame.displayTime.value.count() <= 0)
		return XR_ERROR_VALIDATION_FAILURE;
	if (frame.blendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
		return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
	std::array<std::array<float, 4>, 2> eyeTangents{};
	for (std::size_t eye = 0; eye < target.eyes.size(); ++eye)
		if (!NormalizedPose(target.eyes[eye].worldPose.pose) || !FovTangents(target.eyes[eye].fov, eyeTangents[eye]))
			return XR_ERROR_POSE_INVALID;
	CompositionPlan plan{target.width, target.height, {}, {}};
	for (const auto& eye : target.eyes)
		plan.views.push_back({*NormalizedPose(eye.worldPose.pose), eye.fov});
	for (const auto& layer : frame.layers)
	{
		if (const auto* projection = std::get_if<ProjectionLayerDescription>(&layer))
		{
			if (!ValidLayerFlags(projection->space.flags) ||
				(projection->views.size() != 2 && projection->views.size() != 4))
				return XR_ERROR_LAYER_INVALID;
			for (std::size_t view = 0; view < projection->views.size(); ++view)
			{
				const auto eye = static_cast<std::uint32_t>(view % 2);
				const auto& source = projection->views[view];
				CompositionDraw draw{source.color, eye, {}};
				draw.constants.eyeTangents = eyeTangents[eye];
				if (!NormalizedPose(source.worldPose.pose) || !FovTangents(source.fov, draw.constants.sourceTangents))
					return XR_ERROR_POSE_INVALID;
				auto result = ImageConstants(source.color, resolve, draw.constants);
				if (XR_FAILED(result))
					return result;
				if (EmptyImage(source.color) || !(source.worldPose.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) ||
					!(target.eyes[eye].worldPose.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
					continue;
				if (!SourceFromEye(source.worldPose, target.eyes[eye].worldPose, draw.constants))
					return XR_ERROR_POSE_INVALID;
				draw.constants.control = {0, AlphaFlags(projection->space.flags), 0, 0};
				plan.draws.push_back(draw);
			}
		}
		else
		{
			const auto& quad = std::get<QuadLayerDescription>(layer);
			if (!ValidLayerFlags(quad.space.flags) || !std::isfinite(quad.size.width) ||
				!std::isfinite(quad.size.height) || quad.size.width < 0 || quad.size.height < 0 ||
				(quad.eyes != XR_EYE_VISIBILITY_BOTH && quad.eyes != XR_EYE_VISIBILITY_LEFT &&
				 quad.eyes != XR_EYE_VISIBILITY_RIGHT))
				return XR_ERROR_LAYER_INVALID;
			if (!NormalizedPose(quad.worldPose.pose))
				return XR_ERROR_POSE_INVALID;
			for (std::uint32_t eye = 0; eye < 2; ++eye)
			{
				CompositionDraw draw{quad.color, eye, {}};
				auto result = ImageConstants(quad.color, resolve, draw.constants);
				if (XR_FAILED(result))
					return result;
				if ((quad.eyes == XR_EYE_VISIBILITY_LEFT && eye != 0) ||
					(quad.eyes == XR_EYE_VISIBILITY_RIGHT && eye != 1) || quad.size.width == 0 ||
					quad.size.height == 0 || EmptyImage(quad.color) ||
					(quad.worldPose.flags & ValidPoseFlags) != ValidPoseFlags ||
					(target.eyes[eye].worldPose.flags & ValidPoseFlags) != ValidPoseFlags)
					continue;
				draw.constants.eyeTangents = eyeTangents[eye];
				if (!SourceFromEye(quad.worldPose, target.eyes[eye].worldPose, draw.constants))
					return XR_ERROR_POSE_INVALID;
				draw.constants.quadParameters = {1 / quad.size.width, 1 / quad.size.height, 0, 0};
				if (!std::isfinite(draw.constants.quadParameters[0]) ||
					!std::isfinite(draw.constants.quadParameters[1]))
					return XR_ERROR_LAYER_INVALID;
				draw.constants.control = {1, AlphaFlags(quad.space.flags), 0, 0};
				plan.draws.push_back(draw);
			}
		}
	}
	output = std::move(plan);
	return XR_SUCCESS;
}

std::optional<XrVector2f> EvaluateCompositionSample(const CompositionDraw& draw, XrVector2f eyeUV)
{
	return EvaluateCompositionSample(draw.constants, eyeUV);
}

std::optional<XrVector2f> EvaluateCompositionSample(const LayerConstants& constants, XrVector2f eyeUV)
{
	if (!std::isfinite(eyeUV.x) || !std::isfinite(eyeUV.y) || eyeUV.x < 0 || eyeUV.x > 1 || eyeUV.y < 0 || eyeUV.y > 1)
		return {};
	const XrVector3f eyeRay{constants.eyeTangents[0] + eyeUV.x * (constants.eyeTangents[1] - constants.eyeTangents[0]),
							constants.eyeTangents[2] + eyeUV.y * (constants.eyeTangents[3] - constants.eyeTangents[2]),
							-1};
	const auto& rotation = constants.sourceFromEyeRotation;
	const auto ray = Rotate({rotation[0], rotation[1], rotation[2], rotation[3]}, eyeRay);
	XrVector2f sourceUV;
	if (constants.control[0] == 0)
	{
		if (ray.z >= 0)
			return {};
		sourceUV = {(ray.x / -ray.z - constants.sourceTangents[0]) /
						(constants.sourceTangents[1] - constants.sourceTangents[0]),
					(constants.sourceTangents[2] - ray.y / -ray.z) /
						(constants.sourceTangents[2] - constants.sourceTangents[3])};
	}
	else if (constants.control[0] == 1)
	{
		const auto& position = constants.sourceFromEyePosition;
		if (position[2] <= 0 || ray.z >= 0)
			return {};
		const auto distance = -position[2] / ray.z;
		sourceUV = {(position[0] + distance * ray.x) * constants.quadParameters[0] + 0.5f,
					0.5f - (position[1] + distance * ray.y) * constants.quadParameters[1]};
	}
	else
		return {};
	if (!std::isfinite(sourceUV.x) || !std::isfinite(sourceUV.y) || sourceUV.x < 0 || sourceUV.x > 1 ||
		sourceUV.y < 0 || sourceUV.y > 1)
		return {};
	return XrVector2f{constants.imageRectangle[0] + sourceUV.x * constants.imageRectangle[2],
					  constants.imageRectangle[1] + sourceUV.y * constants.imageRectangle[3]};
}
XrColor4f CompositeReferenceColor(XrColor4f source, XrColor4f destination, std::uint32_t alphaFlags)
{
	if (!(alphaFlags & 1))
		source.a = 1;
	else if (alphaFlags & 2)
	{
		source.r *= source.a;
		source.g *= source.a;
		source.b *= source.a;
	}
	const float retained = 1 - source.a;
	return {source.r + retained * destination.r, source.g + retained * destination.g,
			source.b + retained * destination.b, source.a + retained * destination.a};
}
} // namespace simxr
