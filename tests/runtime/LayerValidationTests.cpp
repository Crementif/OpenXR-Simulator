#include "runtime/validation/LayerValidation.h"
#include "runtime/validation/StructureChain.h"
#include "math/Math.h"
#include "../TestSupport.h"

#include <array>
#include <iostream>
#include <limits>

int main()
{
	using namespace simxr;
	auto image = reinterpret_cast<XrSwapchain>(1);
	auto space = reinterpret_cast<XrSpace>(2);
	LayerValidationContext context;
	context.validSpace = [&](XrSpace handle) { return handle == space; };
	context.swapchain = [&](XrSwapchain handle) -> std::optional<SwapchainDescription>
	{
		if (handle != image)
			return {};
		return SwapchainDescription{128, 64, 2, true};
	};
	std::array<XrCompositionLayerProjectionView, 2> views{};
	for (auto& view : views)
	{
		view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
		view.pose = IdentityPose();
		view.fov = {-0.7f, 0.8f, 0.6f, -0.9f};
		view.subImage = {image, {{4, 8}, {64, 32}}, 1};
	}
	XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, space, 2, views.data()};
	std::array<const XrCompositionLayerBaseHeader*, 1> layers{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection)};
	XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO, nullptr, 123, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, layers.data()};
	Require(ValidateLayers(info, context) == XR_SUCCESS, "Valid stereo layer");
	views[0].fov = {0.7f, -0.8f, -0.6f, 0.9f};
	Require(ValidateLayers(info, context) == XR_SUCCESS, "Flipped field of view rejected");
	views[0].subImage.imageRect.offset.x = std::numeric_limits<std::int32_t>::max();
	Require(ValidateLayers(info, context) == XR_ERROR_SWAPCHAIN_RECT_INVALID, "Rectangle overflow");
	views[0].subImage.imageRect.offset.x = 4;
	views[0].pose.orientation.w = 0;
	Require(ValidateLayers(info, context) == XR_ERROR_POSE_INVALID, "Invalid quaternion");
	views[0].pose = IdentityPose();
	views[0].pose.orientation.w = 1.007f;
	Require(ValidateLayers(info, context) == XR_SUCCESS, "Quaternion norm tolerance");
	views[0].pose.orientation.w = 1.011f;
	Require(ValidateLayers(info, context) == XR_ERROR_POSE_INVALID, "Quaternion tolerance exceeded");
	views[0].pose = IdentityPose();
	views[0].subImage.imageArrayIndex = 2;
	Require(ValidateLayers(info, context) == XR_ERROR_VALIDATION_FAILURE, "Array slice bound");
	views[0].subImage.imageArrayIndex = 1;
	views[0].subImage.imageRect.extent.width = 0;
	Require(ValidateLayers(info, context) == XR_SUCCESS, "Empty image region rejected");
	views[0].subImage.imageRect.extent.width = 64;
	std::array<XrCompositionLayerDepthInfoKHR, 2> depths{};
	for (std::size_t index = 0; index < depths.size(); ++index)
	{
		depths[index] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, nullptr, views[index].subImage, 0, 1, 100, 0.1f};
		views[index].next = &depths[index];
	}
	Require(ValidateLayers(info, context) == XR_ERROR_LAYER_INVALID, "Disabled depth accepted");
	context.depthEnabled = true;
	Require(ValidateLayers(info, context) == XR_SUCCESS, "Reversed depth rejected");
	depths[0].next = &depths[1];
	Require(ValidateLayers(info, context) == XR_ERROR_VALIDATION_FAILURE, "Duplicate known chain type");
	depths[0].next = nullptr;
	depths[0].nearZ = 0;
	Require(ValidateLayers(info, context) == XR_ERROR_LAYER_INVALID, "Zero near plane");
	depths[0].nearZ = std::numeric_limits<float>::infinity();
	Require(ValidateLayers(info, context) == XR_SUCCESS, "Infinite near plane rejected");
	views[1].next = nullptr;
	Require(ValidateLayers(info, context) == XR_ERROR_LAYER_INVALID, "Partial depth layer");
	views[0].next = nullptr;
	XrBaseInStructure unknown{static_cast<XrStructureType>(0x7ffffffe), nullptr};
	views[0].next = &unknown;
	Require(ValidateLayers(info, context) == XR_SUCCESS, "Unknown structure changed feature handling");
	unknown.next = &unknown;
	Require(ValidateLayers(info, context) == XR_ERROR_VALIDATION_FAILURE, "Structure cycle");
	views[0].next = nullptr;
	XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
	quad.space = space;
	quad.pose = IdentityPose();
	quad.subImage = views[0].subImage;
	quad.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	layers[0] = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&quad);
	Require(ValidateLayers(info, context) == XR_SUCCESS, "Zero-sized quad rejected");
	quad.size.width = -1;
	Require(ValidateLayers(info, context) == XR_ERROR_LAYER_INVALID, "Negative quad size");
	layers[0] = nullptr;
	Require(ValidateLayers(info, context) == XR_ERROR_LAYER_INVALID, "Null layer");
	info.layerCount = 0;
	Require(ValidateLayers(info, context) == XR_SUCCESS, "Clear frame rejected");
	std::cout << "Layer validation tests passed\n";
}
