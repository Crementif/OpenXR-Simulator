#include "../TestSupport.h"
#include "graphics/Compositor.h"
#include "graphics/RotationalReprojection.h"
#include "math/Math.h"

#include <cmath>
#include <limits>
#include <numbers>

namespace simxr::tests
{
static void Cylinder()
{
	const auto image = reinterpret_cast<XrSwapchain>(1);
	const auto space = reinterpret_cast<XrSpace>(2);
	XrCompositionLayerCylinderKHR cylinder{XR_TYPE_COMPOSITION_LAYER_CYLINDER_KHR};
	cylinder.space = space;
	cylinder.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
	cylinder.pose = IdentityPose();
	cylinder.radius = 2;
	cylinder.centralAngle = std::numbers::pi_v<float>;
	cylinder.aspectRatio = 2;
	cylinder.subImage = {image, {{16, 8}, {64, 32}}, 1};
	const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&cylinder);
	const XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO, nullptr, 1000, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, &layer};
	LayerValidationContext validation;
	validation.validSpace = [=](XrSpace handle) {
		return handle == space;
	};
	validation.swapchain = [=](XrSwapchain handle) -> std::optional<SwapchainDescription> {
		return handle == image ? std::optional{SwapchainDescription{128, 64, 2, true}} : std::nullopt;
	};
	Require(ValidateLayers(info, validation) == XR_ERROR_LAYER_INVALID, "Disabled cylinder accepted");
	validation.cylinderEnabled = true;
	const PoseSample pose{IdentityPose(), TrackedPoseFlags, {Duration{1000}}};
	std::shared_ptr<const FrameLayers> frame;
	Require(BuildLayers(
				info, validation,
				[&](XrSpace, RuntimeTime, SpaceSample& sample) {
					sample.worldPose = pose;
					sample.referenceType = XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR;
					return XR_SUCCESS;
				},
				frame) == XR_SUCCESS,
			"Build cylinder layer");
	cylinder.radius = -1;
	Require(ValidateLayers(info, validation) == XR_ERROR_LAYER_INVALID, "Negative radius accepted");
	cylinder.radius = std::numeric_limits<float>::quiet_NaN();
	Require(ValidateLayers(info, validation) == XR_ERROR_LAYER_INVALID, "NaN radius accepted");
	cylinder.radius = 0;
	Require(ValidateLayers(info, validation) == XR_SUCCESS, "Infinite cylinder rejected");
	cylinder.centralAngle = 2 * std::numbers::pi_v<float>;
	Require(ValidateLayers(info, validation) == XR_ERROR_LAYER_INVALID, "Out of range angle accepted");
	const auto copied = std::get<CylinderLayerDescription>(frame->layers[0]);
	Require(copied.radius == 2 && FindLayerImage(*frame, 0, 0)->arraySlice == 1 && !FindLayerDepth(*frame, 0, 0),
			"Cylinder image metadata or copied geometry lost");
	const XrFovf fov{-std::atan(1.0f), std::atan(1.0f), std::atan(1.0f), -std::atan(1.0f)};
	CompositionTarget target{64, 64, {{{pose, fov}, {pose, fov}}}};
	const CompositionExtentResolver extent = [](XrSwapchain) {
		return std::optional{XrExtent2Di{128, 64}};
	};
	CompositionPlan plan;
	Require(PrepareComposition(*frame, target, extent, plan) == XR_SUCCESS && plan.draws.size() == 1 &&
				plan.draws[0].eye == 0 && plan.draws[0].source.arraySlice == 1,
			"Cylinder visibility or slice lost");
	auto uv = EvaluateCompositionSample(plan.draws[0], {0.5f, 0.5f});
	Require(uv && std::abs(uv->x - 0.375f) < 0.0001f && std::abs(uv->y - 0.375f) < 0.0001f,
			"Cylinder center/crop mapping wrong");
	uv = EvaluateCompositionSample(plan.draws[0], {1, 0.5f});
	Require(uv && std::abs(uv->x - 0.5f) < 0.0001f, "Cylinder angular mapping wrong");
	Require(!EvaluateCompositionSample(plan.draws[0], {0.5f, 0}), "Cylinder height clipping failed");
	target.eyes[0].worldPose.pose.position.x = 1;
	Require(PrepareComposition(*frame, target, extent, plan) == XR_SUCCESS, "Translated cylinder eye");
	uv = EvaluateCompositionSample(plan.draws[0], {0.5f, 0.5f});
	Require(uv && std::abs(uv->x - (0.375f + 1.0f / 12)) < 0.0001f, "Cylinder ignored eye translation");
	for (float radius : {0.0f, std::numeric_limits<float>::infinity()})
	{
		auto infinite = copied;
		infinite.radius = radius;
		FrameLayers layers{frame->displayTime, frame->blendMode, {infinite}};
		Require(PrepareComposition(layers, target, extent, plan) == XR_SUCCESS, "Infinite cylinder plan");
		uv = EvaluateCompositionSample(plan.draws[0], {0.5f, 0.5f});
		Require(uv && std::abs(uv->x - 0.375f) < 0.0001f, "Infinite cylinder used translation");
	}
	Require(PrepareRotationalComposition(*frame, target, pose, pose, extent, plan) == XR_SUCCESS,
			"Cylinder reprojection rejected local floor space");
}
}

int main()
{
	simxr::tests::Cylinder();
}
