#include "runtime/validation/LayerValidation.h"
#include "runtime/validation/StructureChain.h"

#include <array>
#include <cmath>
#include <cstdint>
#include <numbers>

namespace simxr
{
namespace
{
bool ValidPose(const XrPosef& pose)
{
	const auto& q = pose.orientation;
	float norm = q.x * q.x + q.y * q.y + q.z * q.z + q.w * q.w;
	return std::isfinite(norm) && std::abs(std::sqrt(norm) - 1.0f) <= 0.01f &&
		std::isfinite(pose.position.x) && std::isfinite(pose.position.y) && std::isfinite(pose.position.z);
}

bool ValidFov(const XrFovf& fov)
{
	constexpr float halfPi = std::numbers::pi_v<float> / 2;
	return std::isfinite(fov.angleLeft) && std::isfinite(fov.angleRight) &&
		std::isfinite(fov.angleUp) && std::isfinite(fov.angleDown) &&
		fov.angleLeft != fov.angleRight && fov.angleDown != fov.angleUp &&
		std::abs(fov.angleLeft) < halfPi && std::abs(fov.angleRight) < halfPi &&
		std::abs(fov.angleDown) < halfPi && std::abs(fov.angleUp) < halfPi;
}

XrResult ValidateImage(const XrSwapchainSubImage& image, const LayerValidationContext& context)
{
	auto chain = context.swapchain(image.swapchain);
	if (!chain)
		return XR_ERROR_HANDLE_INVALID;
	if (!chain->released)
		return XR_ERROR_LAYER_INVALID;
	if (image.imageArrayIndex >= chain->arraySize)
		return XR_ERROR_VALIDATION_FAILURE;
	const auto& rect = image.imageRect;
	if (rect.offset.x < 0 || rect.offset.y < 0 || rect.extent.width < 0 || rect.extent.height < 0 ||
		static_cast<std::uint64_t>(rect.offset.x) + rect.extent.width > chain->width ||
		static_cast<std::uint64_t>(rect.offset.y) + rect.extent.height > chain->height)
		return XR_ERROR_SWAPCHAIN_RECT_INVALID;
	return XR_SUCCESS;
}

XrResult ValidateProjection(const XrCompositionLayerProjection& layer, const LayerValidationContext& context)
{
	if (layer.viewCount != context.viewCount || !layer.views)
		return XR_ERROR_LAYER_INVALID;
	bool hasDepth = false;
	for (std::uint32_t index = 0; index < layer.viewCount; ++index)
	{
		const auto& view = layer.views[index];
		if (view.type != XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW || !ValidFov(view.fov))
			return XR_ERROR_LAYER_INVALID;
		if (!ValidPose(view.pose))
			return XR_ERROR_POSE_INVALID;
		auto result = ValidateImage(view.subImage, context);
		if (XR_FAILED(result))
			return result;
		StructureChain chain(view.next);
		constexpr std::array knownTypes{XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR};
		result = chain.Validate(knownTypes);
		if (XR_FAILED(result))
			return result;
		auto depth = chain.Find<XrCompositionLayerDepthInfoKHR>(XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR);
		if (index == 0)
			hasDepth = depth != nullptr;
		else if (hasDepth != (depth != nullptr))
			return XR_ERROR_LAYER_INVALID;
		if (depth)
		{
			if (!context.depthEnabled)
				return XR_ERROR_LAYER_INVALID;
			if (!std::isfinite(depth->minDepth) || !std::isfinite(depth->maxDepth) ||
				depth->minDepth < 0 || depth->maxDepth > 1 || depth->minDepth >= depth->maxDepth ||
				std::isnan(depth->nearZ) || std::isnan(depth->farZ) || depth->nearZ <= 0 || depth->farZ <= 0 || depth->nearZ == depth->farZ)
				return XR_ERROR_LAYER_INVALID;
			result = ValidateImage(depth->subImage, context);
			if (XR_FAILED(result))
				return result;
		}
	}
	return XR_SUCCESS;
}
}

XrResult ValidateLayers(const XrFrameEndInfo& info, const LayerValidationContext& context)
{
	if (info.type != XR_TYPE_FRAME_END_INFO || (info.layerCount && !info.layers))
		return XR_ERROR_VALIDATION_FAILURE;
	if (info.displayTime <= 0)
		return XR_ERROR_TIME_INVALID;
	if (info.environmentBlendMode != XR_ENVIRONMENT_BLEND_MODE_OPAQUE)
		return XR_ERROR_ENVIRONMENT_BLEND_MODE_UNSUPPORTED;
	if (info.layerCount > context.maxLayers)
		return XR_ERROR_LAYER_LIMIT_EXCEEDED;
	for (std::uint32_t index = 0; index < info.layerCount; ++index)
	{
		auto layer = info.layers[index];
		if (!layer)
			return XR_ERROR_LAYER_INVALID;
		if (!context.validSpace(layer->space))
			return XR_ERROR_HANDLE_INVALID;
		constexpr XrCompositionLayerFlags knownFlags = XR_COMPOSITION_LAYER_CORRECT_CHROMATIC_ABERRATION_BIT |
			XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
		if (layer->layerFlags & ~knownFlags)
			return XR_ERROR_VALIDATION_FAILURE;
		StructureChain chain(layer->next);
		auto result = chain.Validate({});
		if (XR_FAILED(result))
			return result;
		if (layer->type == XR_TYPE_COMPOSITION_LAYER_PROJECTION)
			result = ValidateProjection(*reinterpret_cast<const XrCompositionLayerProjection*>(layer), context);
		else if (layer->type == XR_TYPE_COMPOSITION_LAYER_QUAD)
		{
			const auto& quad = *reinterpret_cast<const XrCompositionLayerQuad*>(layer);
			if (!ValidPose(quad.pose))
				return XR_ERROR_POSE_INVALID;
			if (!std::isfinite(quad.size.width) || !std::isfinite(quad.size.height) || quad.size.width < 0 || quad.size.height < 0 ||
				(quad.eyeVisibility != XR_EYE_VISIBILITY_BOTH && quad.eyeVisibility != XR_EYE_VISIBILITY_LEFT && quad.eyeVisibility != XR_EYE_VISIBILITY_RIGHT))
				return XR_ERROR_LAYER_INVALID;
			result = ValidateImage(quad.subImage, context);
		}
		else
			return XR_ERROR_LAYER_INVALID;
		if (XR_FAILED(result))
			return result;
	}
	return XR_SUCCESS;
}
}
