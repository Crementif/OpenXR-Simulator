#include "../TestSupport.h"
#include "Math.h"
#include "graphics/Compositor.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>

namespace
{
using namespace simxr;
void Near(float actual, float expected, const char* message)
{
	Require(std::abs(actual - expected) < 0.0001f, message);
}
XrFovf Fov(float left = -1, float right = 1, float up = 1, float down = -1)
{
	return {std::atan(left), std::atan(right), std::atan(up), std::atan(down)};
}
PoseSample Pose(XrVector3f position = {})
{
	return {{{0, 0, 0, 1}, position}, TrackedPoseFlags, {Duration{1000}}};
}
struct Fixture
{
	XrSwapchain source = reinterpret_cast<XrSwapchain>(1);
	CompositionTarget target{640, 480, {{{Pose(), Fov()}, {Pose(), Fov()}}}};
	CompositionExtentResolver extent = [this](XrSwapchain handle) -> std::optional<XrExtent2Di>
	{ return handle == source ? std::optional{XrExtent2Di{128, 64}} : std::nullopt; };
	LayerImageDescription Image(std::uint32_t slice = 0)
	{
		return {source, {{0, 0}, {128, 64}}, slice};
	}
	ProjectionLayerDescription Projection(std::size_t views = 2)
	{
		ProjectionLayerDescription layer;
		for (std::size_t i = 0; i < views; ++i)
			layer.views.push_back({IdentityPose(), Pose(), Fov(), Image(static_cast<std::uint32_t>(i)), {}});
		return layer;
	}
	QuadLayerDescription Quad()
	{
		return {{}, XR_EYE_VISIBILITY_BOTH, IdentityPose(), Pose({0, 0, -2}), {2, 2}, Image()};
	}
	FrameLayers Frame(std::vector<LayerDescription> layers)
	{
		return {{Duration{1000}}, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, std::move(layers)};
	}
};
void Projection()
{
	Fixture f;
	auto layer = f.Projection();
	f.target.eyes[0].fov = Fov(-0.5f, 1.5f, 1, -0.25f);
	layer.views[0].color.rectangle = {{32, 8}, {64, 32}};
	CompositionPlan plan;
	Require(PrepareComposition(f.Frame({layer}), f.target, f.extent, plan) == XR_SUCCESS && plan.width == 640 &&
				plan.height == 480 && plan.draws.size() == 2,
			"Stereo plan dimensions/draws");
	auto uv = EvaluateCompositionSample(plan.draws[0], {0.5f, 0.5f});
	Require(uv.has_value(), "Asymmetric eye ray missed source");
	Near(uv->x, 0.625f, "Asymmetric projection/crop U");
	Near(uv->y, 0.28125f, "Asymmetric projection/crop V");
	Require(!EvaluateCompositionSample(plan.draws[0], {1, 0.5f}), "Outside projection FOV sampled edge");
	f.target.eyes[0].fov = Fov();
	const float yaw = std::numbers::pi_v<float> / 6;
	layer.views[0].color = f.Image();
	layer.views[0].worldPose.pose.orientation = EulerAngles(yaw, 0, 0);
	Require(PrepareComposition(f.Frame({layer}), f.target, f.extent, plan) == XR_SUCCESS, "Rotated projection plan");
	uv = EvaluateCompositionSample(plan.draws[0], {0.5f, 0.5f});
	Near(uv->x, 0.5f + std::tan(yaw) / 2, "Inverse source orientation projection");
	layer.views[0].worldPose.pose.position = {100, 200, 300};
	f.target.eyes[0].worldPose.pose.orientation = layer.views[0].worldPose.pose.orientation;
	Require(PrepareComposition(f.Frame({layer}), f.target, f.extent, plan) == XR_SUCCESS,
			"Aligned source/eye orientation");
	uv = EvaluateCompositionSample(plan.draws[0], {0.5f, 0.5f});
	Near(uv->x, 0.5f, "Projection incorrectly used positional reprojection without depth");
	layer.views[0].worldPose.flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	Require(PrepareComposition(f.Frame({layer}), f.target, f.extent, plan) == XR_SUCCESS && plan.draws.size() == 2,
			"Valid untracked orientation omitted projection");
	layer.views[0].worldPose.flags = XR_SPACE_LOCATION_POSITION_VALID_BIT;
	Require(PrepareComposition(f.Frame({layer}), f.target, f.extent, plan) == XR_SUCCESS && plan.draws.size() == 1 &&
				plan.draws[0].eye == 1,
			"Invalid orientation fabricated projection");
}
void Quad()
{
	Fixture f;
	auto quad = f.Quad();
	quad.eyes = XR_EYE_VISIBILITY_LEFT;
	quad.worldPose.pose.orientation = EulerAngles(std::numbers::pi_v<float> / 6, 0, 0);
	CompositionPlan plan;
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_SUCCESS && plan.draws.size() == 1 &&
				plan.draws[0].eye == 0,
			"Quad per-eye visibility");
	const auto point = Compose(quad.worldPose.pose, {{0, 0, 0, 1}, {0.5f, 0.5f, 0}}).position;
	const XrVector2f eyeUV{(point.x / -point.z + 1) / 2, (1 - point.y / -point.z) / 2};
	auto uv = EvaluateCompositionSample(plan.draws[0], eyeUV);
	Require(uv.has_value(), "Rotated quad ray missed front face");
	Near(uv->x, 0.75f, "Rotated quad plane intersection U");
	Near(uv->y, 0.25f, "Rotated quad top-left V");
	Require(!EvaluateCompositionSample(plan.draws[0], {0, 0}), "Ray outside physical quad sampled edge");
	quad.worldPose.pose.orientation = EulerAngles(std::numbers::pi_v<float>, 0, 0);
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_SUCCESS &&
				!EvaluateCompositionSample(plan.draws[0], {0.5f, 0.5f}),
			"Back-facing quad rendered");
	quad = f.Quad();
	quad.worldPose.pose.position.x = 1;
	f.target.eyes[0].worldPose.pose.position.x = 0.5f;
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_SUCCESS, "Translated eye/quad plan");
	uv = EvaluateCompositionSample(plan.draws[0], {0.5f, 0.5f});
	Near(uv->x, 0.25f, "Quad did not use relative eye position");
	quad.worldPose.flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_SUCCESS && plan.draws.empty(),
			"Quad without valid position rendered");
}
void OrderingAndAlpha()
{
	Fixture f;
	auto inset = f.Projection(4);
	inset.views[2].fov = inset.views[3].fov = Fov(-0.25f, 0.25f, 0.25f, -0.25f);
	inset.space.flags =
		XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT;
	auto quad = f.Quad();
	quad.eyes = XR_EYE_VISIBILITY_RIGHT;
	CompositionPlan plan;
	Require(PrepareComposition(f.Frame({inset, quad}), f.target, f.extent, plan) == XR_SUCCESS &&
				plan.draws.size() == 5,
			"Mixed inset plan");
	for (std::size_t i = 0; i < 4; ++i)
		Require(plan.draws[i].eye == i % 2 && plan.draws[i].source.arraySlice == i &&
					plan.draws[i].constants.control[1] == 3,
				"Inset/outer draw order, slice, or straight-alpha intent changed");
	Require(plan.draws[4].eye == 1 && plan.draws[4].constants.control[0] == 1 &&
				plan.draws[4].constants.control[1] == 0,
			"Quad layer order or opaque intent changed");
	Require(EvaluateCompositionSample(plan.draws[0], {0.1f, 0.5f}) &&
				!EvaluateCompositionSample(plan.draws[2], {0.1f, 0.5f}) &&
				EvaluateCompositionSample(plan.draws[2], {0.5f, 0.5f}),
			"Inset did not preserve outer image outside inset FOV");
	const auto straight = CompositeReferenceColor({0.8f, 0.4f, 0.2f, 0.25f}, {0.1f, 0.2f, 0.3f, 1}, 3);
	Near(straight.r, 0.275f, "Straight alpha must premultiply before linear blend");
	Near(straight.g, 0.25f, "Linear source-over green");
	Near(straight.b, 0.275f, "Linear source-over blue");
	Near(straight.a, 1, "Source-over output alpha");
	const auto premultiplied = CompositeReferenceColor({0.2f, 0.1f, 0.05f, 0.25f}, {0.1f, 0.2f, 0.3f, 1}, 1);
	Near(premultiplied.r, straight.r, "Premultiplied input was multiplied twice");
	const auto opaque = CompositeReferenceColor({0.8f, 0.4f, 0.2f, 0.25f}, {1, 1, 1, 1}, 2);
	Near(opaque.r, 0.8f, "No-source-alpha flag must ignore stored alpha");
	Near(opaque.a, 1, "Opaque alpha intent");
	const float decodedHalf = std::pow((0.5f + 0.055f) / 1.055f, 2.4f);
	const auto linear = CompositeReferenceColor({decodedHalf, decodedHalf, decodedHalf, 0.5f}, {0, 0, 0, 1}, 3);
	Near(linear.r, 0.10702057f, "sRGB view must decode before linear alpha composition");
}
void QuadCropBounds()
{
	Fixture fixture;
	fixture.target.width = 96;
	fixture.target.height = 64;
	for (int scenario = 0; scenario < 4; ++scenario)
	{
		auto quad = fixture.Quad();
		quad.eyes = XR_EYE_VISIBILITY_RIGHT;
		quad.worldPose.pose.position.x = scenario == 1 ? 1.8f : 0.2f;
		quad.worldPose.pose.position.z = scenario == 2 ? -0.1f : scenario == 3 ? 2.0f : -2.0f;
		quad.worldPose.pose.orientation = EulerAngles(0.4f, 0.2f, 0.1f);
		CompositionPlan plan;
		Require(PrepareComposition(fixture.Frame({fixture.Projection(), quad}), fixture.target, fixture.extent, plan) == XR_SUCCESS,
			"Prepare cropped quad fixture");
		const auto bounds = CompositionQuadBounds(plan);
		Require(bounds[0].extent.width == 0, "Projection or wrong-eye geometry contaminated UI bounds");
		const auto& rectangle = bounds[1];
		for (std::int32_t y = 0; y < 64; ++y)
			for (std::int32_t x = 0; x < 96; ++x)
				if (EvaluateCompositionSample(plan.draws.back(), {(x + 0.5f) / 96, (y + 0.5f) / 64}))
					Require(x >= rectangle.offset.x && y >= rectangle.offset.y &&
						x < rectangle.offset.x + rectangle.extent.width && y < rectangle.offset.y + rectangle.extent.height,
						"Historical crop excluded a pixel accepted by the compositor ray equation");
		if (scenario == 3)
			Require(rectangle.extent.width == 0, "Behind-eye quad produced a visible UI rectangle");
	}
}
void Rejection()
{
	Fixture f;
	auto quad = f.Quad();
	CompositionPlan plan;
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_SUCCESS, "Rejection baseline");
	quad.color.rectangle.extent.width = 129;
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_ERROR_SWAPCHAIN_RECT_INVALID &&
				plan.draws.size() == 2,
			"Invalid crop changed previous plan");
	quad = f.Quad();
	quad.size.width = -1;
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_ERROR_LAYER_INVALID,
			"Negative quad size accepted");
	quad = f.Quad();
	quad.worldPose.pose.orientation.w = 0;
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_ERROR_POSE_INVALID,
			"Malformed quaternion accepted");
	quad = f.Quad();
	quad.color.rectangle.extent.width = 0;
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_SUCCESS && plan.draws.empty(),
			"Zero crop generated draw");
	quad = f.Quad();
	quad.size.height = 0;
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_SUCCESS && plan.draws.empty(),
			"Zero quad generated draw");
	auto projection = f.Projection(3);
	Require(PrepareComposition(f.Frame({projection}), f.target, f.extent, plan) == XR_ERROR_LAYER_INVALID,
			"Unsupported view count accepted");
	projection = f.Projection();
	projection.views[0].fov.angleUp = std::numeric_limits<float>::quiet_NaN();
	Require(PrepareComposition(f.Frame({projection}), f.target, f.extent, plan) == XR_ERROR_POSE_INVALID,
			"Nonfinite FOV accepted");
	quad = f.Quad();
	quad.size.width = std::numeric_limits<float>::denorm_min();
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_ERROR_LAYER_INVALID,
			"Unrepresentable quad reciprocal accepted");
	quad = f.Quad();
	quad.worldPose.pose.position.x = std::numeric_limits<float>::max();
	f.target.eyes[0].worldPose.pose.position.x = -std::numeric_limits<float>::max();
	Require(PrepareComposition(f.Frame({quad}), f.target, f.extent, plan) == XR_ERROR_POSE_INVALID,
			"Overflowed relative pose accepted");
	quad = f.Quad();
	f.target.eyes[0].worldPose = Pose();
	Require(PrepareComposition(f.Frame({quad}), f.target, {}, plan) == XR_ERROR_HANDLE_INVALID,
			"Missing extent resolver accepted image");
	Require(PrepareComposition(f.Frame({}), f.target, {}, plan) == XR_SUCCESS && plan.draws.empty(),
			"Empty composition requires no source resolver");
}
} // namespace
int main()
{
	Projection();
	Quad();
	OrderingAndAlpha();
	Rejection();
	QuadCropBounds();
	std::cout << "Compositor plan/ray/linear-alpha reference tests passed; no GPU execution claimed\n";
}
