#include "../TestSupport.h"
#include "Math.h"
#include "graphics/RotationalReprojection.h"
#include "simulation/SpaceSystem.h"

#include <array>
#include <cmath>
#include <iostream>
#include <limits>

namespace
{
using namespace simxr;
void Near(float actual, float expected, const char* message)
{
	Require(std::abs(actual - expected) < 0.0001f, message);
}
XrFovf Fov(float tangent = 1)
{
	const auto angle = std::atan(tangent);
	return {-angle, angle, angle, -angle};
}
struct Fixture
{
	TrackingSystem tracking;
	SpaceSystem spaces{reinterpret_cast<XrSession>(1), tracking};
	XrSwapchain image = reinterpret_cast<XrSwapchain>(2);
	XrSpace local = XR_NULL_HANDLE;
	XrSpace view = XR_NULL_HANDLE;
	PoseSample head;
	CompositionTarget target{128, 64, {}};
	std::shared_ptr<const FrameLayers> frame;
	CompositionExtentResolver extent = [this](XrSwapchain source) -> std::optional<XrExtent2Di>
	{
		return source == image ? std::optional{XrExtent2Di{128, 64}} : std::nullopt;
	};

	explicit Fixture(XrPosef pose = IdentityPose(), bool viewProjection = false)
	{
		head = {pose, TrackedPoseFlags, {Duration{1000}}};
		TrackingState state;
		state.head = head;
		tracking.Publish(head.sampleTime, state);
		Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, IdentityPose(), local) == XR_SUCCESS,
			"Create world reference");
		const XrPosef offset{EulerAngles(0.13f, 0.04f, 0.02f), {0.1f, 0.05f, -0.2f}};
		Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, offset, view) == XR_SUCCESS,
			"Create offset VIEW reference");
		std::array<XrCompositionLayerProjectionView, 4> views{};
		for (std::uint32_t index = 0; index < 4; ++index)
		{
			const auto eye = index % 2;
			const XrPosef eyeOffset{{0, 0, 0, 1}, {eye ? 0.032f : -0.032f, 0, 0}};
			target.eyes[eye] = {{Compose(head.pose, eyeOffset), TrackedPoseFlags, head.sampleTime}, Fov()};
			views[index] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW, nullptr,
				target.eyes[eye].worldPose.pose, Fov(index < 2 ? 1 : 0.25f),
				{image, {{16, 8}, {64, 32}}, index}};
		}
		XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr,
			XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT | XR_COMPOSITION_LAYER_UNPREMULTIPLIED_ALPHA_BIT,
			viewProjection ? view : local, 4, views.data()};
		XrCompositionLayerQuad hud{XR_TYPE_COMPOSITION_LAYER_QUAD, nullptr,
			XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT, view, XR_EYE_VISIBILITY_BOTH,
			{image, {{0, 0}, {128, 64}}, 1}, {{0, 0, 0, 1}, {0, 0, -2}}, {2, 2}};
		auto worldQuad = hud;
		worldQuad.space = local;
		worldQuad.eyeVisibility = XR_EYE_VISIBILITY_RIGHT;
		worldQuad.pose = Compose(head.pose, hud.pose);
		std::array layers{reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection),
			reinterpret_cast<const XrCompositionLayerBaseHeader*>(&hud),
			reinterpret_cast<const XrCompositionLayerBaseHeader*>(&worldQuad)};
		XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, 1000, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
			static_cast<std::uint32_t>(layers.size()), layers.data()};
		LayerValidationContext validation;
		validation.viewCount = 4;
		validation.validSpace = [this](XrSpace space) { return spaces.Owns(space); };
		validation.swapchain = [this](XrSwapchain source) -> std::optional<SwapchainDescription>
		{
			return source == image ? std::optional{SwapchainDescription{128, 64, 4, true}} : std::nullopt;
		};
		Require(BuildLayers(end, validation, [this](XrSpace space, RuntimeTime time, SpaceSample& sample)
			{ return spaces.Sample(space, time, sample); }, frame) == XR_SUCCESS, "Snapshot mixed layers");
	}
	CompositionPlan Baseline()
	{
		CompositionPlan plan;
		Require(PrepareComposition(*frame, target, extent, plan) == XR_SUCCESS, "Repeat baseline geometry");
		return plan;
	}
	CompositionPlan RotateTo(PoseSample display)
	{
		CompositionPlan plan;
		Require(PrepareRotationalComposition(*frame, target, head, display, extent, plan) == XR_SUCCESS,
			"Prepare rotational geometry");
		return plan;
	}
};
void SameSamples(const CompositionDraw& actual, const CompositionDraw& expected)
{
	Require(actual.eye == expected.eye && actual.source.source == expected.source.source &&
		actual.source.arraySlice == expected.source.arraySlice &&
		actual.constants.control == expected.constants.control &&
		actual.constants.eyeTangents == expected.constants.eyeTangents &&
		actual.constants.sourceTangents == expected.constants.sourceTangents &&
		actual.constants.imageRectangle == expected.constants.imageRectangle,
		"Rotation changed draw identity or optics");
	for (const XrVector2f uv : {XrVector2f{0.4f, 0.4f}, {0.5f, 0.5f}, {0.6f, 0.6f}})
	{
		const auto a = EvaluateCompositionSample(actual, uv);
		const auto b = EvaluateCompositionSample(expected, uv);
		Require(a.has_value() == b.has_value(), "Rotation changed stable layer coverage");
		if (a)
		{
			Near(a->x, b->x, "Stable layer U changed");
			Near(a->y, b->y, "Stable layer V changed");
		}
	}
}
void KnownRotation()
{
	Fixture fixture;
	const auto baseline = fixture.Baseline();
	Require(baseline.draws.size() == 7, "Outer/inset, HUD and world quad draw count");
	auto display = fixture.head;
	const float yaw = 0.2f;
	display.pose.orientation = EulerAngles(yaw, 0, 0);
	display.sampleTime = {Duration{4000}};
	const auto rotated = fixture.RotateTo(display);
	const auto outer = EvaluateCompositionSample(rotated.draws[0], {0.5f, 0.5f});
	const auto inset = EvaluateCompositionSample(rotated.draws[2], {0.5f, 0.5f});
	Require(outer && inset, "Known yaw lost valid source rays");
	Near(outer->x, 0.125f + 0.5f * (0.5f - 0.5f * std::tan(yaw)), "World projection yaw direction/crop");
	Near(inset->x, 0.125f + 0.5f * (0.5f - 2 * std::tan(yaw)), "Inset FOV correction");
	SameSamples(rotated.draws[4], baseline.draws[4]);
	SameSamples(rotated.draws[5], baseline.draws[5]);
	const auto quad = EvaluateCompositionSample(rotated.draws[6], {0.5f, 0.5f});
	Require(quad.has_value(), "Known yaw lost world quad");
	Near(quad->x, 0.5f + 0.032f / (2 * std::cos(yaw)) - std::tan(yaw), "World quad eye rotation about head center");
	display.pose.position = {1000, -2000, 3000};
	display.flags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	const auto translated = fixture.RotateTo(display);
	for (std::size_t index = 0; index < rotated.draws.size(); ++index)
		SameSamples(translated.draws[index], rotated.draws[index]);
	const auto unchanged = fixture.Baseline();
	for (std::size_t index = 0; index < baseline.draws.size(); ++index)
		SameSamples(unchanged.draws[index], baseline.draws[index]);
}
void SpaceLifetimeAndGeneralRotation()
{
	Fixture fixture({EulerAngles(0.3f, 0.2f, -0.1f), {2, 1.5f, -3}});
	const auto baseline = fixture.Baseline();
	const auto& saved = std::get<QuadLayerDescription>(fixture.frame->layers[1]).space;
	Require(saved.referenceType == XR_REFERENCE_SPACE_TYPE_VIEW && saved.poseOffset.position.x == 0.1f,
		"Submission omitted immutable space type/offset");
	SpaceSample sample;
	Require(fixture.spaces.Sample(fixture.view, fixture.head.sampleTime, sample) == XR_SUCCESS,
		"Sample VIEW identity and pose together");
	fixture.spaces.Clear();
	Require(fixture.spaces.Sample(fixture.view, fixture.head.sampleTime, sample) == XR_ERROR_HANDLE_INVALID &&
		sample.referenceType == XR_REFERENCE_SPACE_TYPE_VIEW && sample.poseOffset.position.x == 0.1f,
		"Failed space sample mutated previous value");
	auto display = fixture.head;
	display.pose.orientation = EulerAngles(0.35f, -0.15f, 0.27f);
	const auto rotated = fixture.RotateTo(display);
	SameSamples(rotated.draws[4], baseline.draws[4]);
	SameSamples(rotated.draws[5], baseline.draws[5]);
	const auto identity = fixture.RotateTo(fixture.head);
	for (std::size_t index = 0; index < baseline.draws.size(); ++index)
		SameSamples(identity.draws[index], baseline.draws[index]);
}
void ViewProjection()
{
	Fixture fixture({EulerAngles(0.2f, -0.1f, 0.05f), {0.1f, 0.2f, -0.1f}}, true);
	fixture.target.eyes[0].fov = {std::atan(-0.6f), std::atan(1.3f), std::atan(0.8f), std::atan(-0.4f)};
	fixture.target.eyes[0].worldPose.pose = Compose(fixture.target.eyes[0].worldPose.pose,
		{EulerAngles(0.06f, 0.02f, 0), {0, 0, 0}});
	const auto baseline = fixture.Baseline();
	auto display = fixture.head;
	display.pose.orientation = EulerAngles(-0.2f, 0.15f, -0.1f);
	const auto rotated = fixture.RotateTo(display);
	for (std::size_t index = 0; index < 6; ++index)
		SameSamples(rotated.draws[index], baseline.draws[index]);
}
void LargeOriginKeepsRelativeEyes()
{
	const float maximum = std::numeric_limits<float>::max();
	const PoseSample submitted{
		{EulerAngles(0, 0, 0.785398163f), {maximum, maximum, 0}}, TrackedPoseFlags, {Duration{100}}};
	auto display = submitted;
	display.pose.orientation = EulerAngles(0.2f, 0, 0);
	CompositionTarget target{128, 64, {{{submitted, Fov()}, {submitted, Fov()}}}};
	ProjectionLayerDescription projection;
	projection.space.referenceType = XR_REFERENCE_SPACE_TYPE_STAGE;
	for (unsigned eye = 0; eye < 2; ++eye)
		projection.views.push_back(
			{submitted.pose, submitted, Fov(), {reinterpret_cast<XrSwapchain>(2), {{0, 0}, {128, 64}}, eye}, {}});
	const FrameLayers layers{{Duration{100}}, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, {projection}};
	const CompositionExtentResolver extent = [](XrSwapchain)
	{
		return XrExtent2Di{128, 64};
	};
	CompositionPlan rotated, expected;
	Require(PrepareRotationalComposition(layers, target, submitted, display, extent, rotated) == XR_SUCCESS,
			"Representable eye poses overflowed through the inverse world origin");
	for (auto& eye : target.eyes)
		eye.worldPose = display;
	Require(PrepareComposition(layers, target, extent, expected) == XR_SUCCESS && rotated.draws.size() == 2,
			"Large-origin reference composition failed");
	for (unsigned eye = 0; eye < 2; ++eye)
		SameSamples(rotated.draws[eye], expected.draws[eye]);
}

void Rejection()
{
	Fixture fixture;
	CompositionPlan output{17, 19, {}};
	auto display = fixture.head;
	display.flags = XR_SPACE_LOCATION_POSITION_VALID_BIT;
	Require(PrepareRotationalComposition(*fixture.frame, fixture.target, fixture.head, display,
		fixture.extent, output) == XR_ERROR_POSE_INVALID, "Lost orientation accepted");
	display = fixture.head;
	display.pose.orientation.w = std::numeric_limits<float>::quiet_NaN();
	Require(PrepareRotationalComposition(*fixture.frame, fixture.target, fixture.head, display,
		fixture.extent, output) == XR_ERROR_POSE_INVALID, "Nonfinite orientation accepted");
	auto descriptions = fixture.frame->layers;
	std::get<QuadLayerDescription>(descriptions[1]).space.referenceType = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
	FrameLayers dynamic{fixture.frame->displayTime, fixture.frame->blendMode, std::move(descriptions)};
	Require(PrepareRotationalComposition(dynamic, fixture.target, fixture.head, fixture.head,
		fixture.extent, output) == XR_ERROR_FEATURE_UNSUPPORTED,
		"Unknown/action space silently treated as world locked");
	Require(output.width == 17 && output.height == 19 && output.draws.empty(), "Rejection replaced prior plan");
}
} // namespace
int main()
{
	KnownRotation();
	SpaceLifetimeAndGeneralRotation();
	ViewProjection();
	LargeOriginKeepsRelativeEyes();
	Rejection();
	std::cout << "Rotational geometry, reference-space lifetime and repeat baseline passed; "
		"GPU policy remains separate\n";
}
