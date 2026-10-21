#include "../TestSupport.h"
#include "Math.h"
#include "graphics/Compositor.h"
#include "graphics/PreviewGeometry.h"
#include "runtime/Instance.h"
#include "runtime/Session.h"

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

QuadLayerDescription Quad()
{
	QuadLayerDescription quad{};
	quad.worldPose = {IdentityPose(), TrackedPoseFlags};
	quad.worldPose.pose.position.z = -2;
	quad.size = {2, 2};
	quad.eyes = XR_EYE_VISIBILITY_BOTH;
	quad.color = {reinterpret_cast<XrSwapchain>(1), {{16, 8}, {32, 16}}, 3};
	return quad;
}

LocatedViews Views()
{
	LocatedViews views;
	views.time = {Duration{1000}};
	for (std::uint32_t eye = 0; eye < 2; ++eye)
		views.views.push_back({{eye, ViewRole::Outer, 100, 100}, {IdentityPose(), TrackedPoseFlags}, Fov()});
	return views;
}

void GeometryAndShaderAbi()
{
	const auto quad = Quad();
	auto views = Views();
	views.views[0].fov = Fov(-0.5f, 1.5f, 1.25f, -0.75f);
	const auto geometry = PreparePreviewQuad(quad, views, {64, 32});
	Require(geometry && geometry->eyes[0] && geometry->eyes[1], "Prepare asymmetric stereo quad");
	Require(geometry->source.source == quad.color.source && geometry->source.arraySlice == 3,
			"Copied source identity lost array slice");
	const auto constants = PreviewQuadConstants(*geometry, 0, true);
	Near(constants[0], -1, "D3D12 TL view x");
	Near(constants[1], 1, "D3D12 TL view y");
	Near(constants[2], -2, "D3D12 TL view depth");
	Near(constants[16], -0.5f, "D3D12 asymmetric left tangent");
	Near(constants[20], 0.25f, "D3D12 crop offset");
	Near(constants[22], 0.75f, "D3D12 crop maximum");
	Near(constants[24], 1, "Opaque option ABI");
	Near(PreviewQuadConstants(*geometry, 1, false)[24], 0, "Source alpha option ABI");
	const auto& strip = geometry->eyes[0]->stripClipCorners;
	Near(strip[0][0], -2, "Asymmetric clip x");
	Near(strip[0][1], 0.5f, "Asymmetric clip y");
	Near(strip[0][3], 2, "Perspective interpolation depth");
	Near(strip[2][1], -1.5f, "Strip third corner must be bottom left");
	const auto bounds = PreviewQuadBounds(*geometry->eyes[1], {10, 20, 80, 60}, {100, 100});
	Require(bounds && bounds->offset.x == 30 && bounds->offset.y == 35 && bounds->extent.width == 40 &&
				bounds->extent.height == 30,
			"Viewport bounds do not match projection");

	FrameLayers frame(views.time, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, {quad});
	CompositionTarget target{
		100, 100, {{{views.views[0].pose, views.views[0].fov}, {views.views[1].pose, views.views[1].fov}}}};
	CompositionPlan plan;
	Require(PrepareComposition(
				frame, target,
				[](XrSwapchain)
				{
					return XrExtent2Di{64, 32};
				},
				plan) == XR_SUCCESS,
			"Canonical reference composition");
	const auto center = EvaluateCompositionSample(plan.draws[1], {0.5f, 0.5f});
	Require(center.has_value(), "Canonical quad center missing");
	Near(center->x, (constants[20] + constants[22]) / 2, "Geometry crop differs from canonical compositor");
	Near(center->y, 0.5f, "Canonical top-left UV center");
}

void HistoricalSubmission()
{
	Instance instance(reinterpret_cast<XrInstance>(1), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(2), instance);
	FrameSchedulerConfig configuration;
	configuration.mode = ExecutionMode::Deterministic;
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(configuration.epoch) == XR_SUCCESS && session.Begin(begin, configuration) == XR_SUCCESS,
			"Begin historical geometry session");
	TrackingState tracking;
	tracking.head.pose.position = {1, 0, 0};
	ViewSettings settings;
	settings.ipd = 0.08f;
	settings.outerFov = {Fov(-0.5f, 1.5f), Fov()};
	session.PublishTracking({Duration{1000}}, tracking, settings);
	XrSpace viewSpace;
	Require(session.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, IdentityPose(), viewSpace) ==
				XR_SUCCESS,
			"Create historical view space");
	XrCompositionLayerQuad submitted{};
	submitted.type = XR_TYPE_COMPOSITION_LAYER_QUAD;
	submitted.space = viewSpace;
	submitted.eyeVisibility = XR_EYE_VISIBILITY_BOTH;
	submitted.pose = Quad().worldPose.pose;
	submitted.size = {2, 2};
	submitted.subImage = {Quad().color.source, Quad().color.rectangle, 3};
	const auto* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&submitted);
	XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO, nullptr, 1000, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, &layer};
	LayerValidationContext validation;
	validation.validSpace = [&](XrSpace space)
	{
		return session.Spaces().Owns(space);
	};
	validation.swapchain = [](XrSwapchain)
	{
		return SwapchainDescription{64, 32, 4, true};
	};
	std::shared_ptr<const FrameLayers> accepted;
	Require(BuildLayers(
				info, validation,
				[&](XrSpace space, RuntimeTime time, SpaceSample& pose)
				{
					return session.Spaces().Sample(space, time, pose);
				},
				accepted) == XR_SUCCESS,
			"Prepare accepted quad at displayTime");
	submitted.pose.position.z = -100;
	tracking.head.pose = {EulerAngles(0.8f, 0.3f, 0), {10, 3, 4}};
	settings.ipd = 0.2f;
	settings.outerFov = {Fov(), Fov()};
	session.PublishTracking({Duration{2000}}, tracking, settings);
	const auto historical = session.ViewsAt(accepted->displayTime);
	const auto& quad = std::get<QuadLayerDescription>(accepted->layers[0]);
	const auto geometry = PreparePreviewQuad(quad, historical, {64, 32});
	Require(geometry && geometry->eyes[0], "Historical quad geometry unavailable");
	Near(geometry->eyes[0]->viewCorners[0].x, -0.96f, "New head/IPD changed historical quad transform");
	Near(geometry->eyes[0]->viewCorners[0].z, -2, "Caller mutation changed accepted world pose");
	Near(geometry->eyes[0]->tangents[0], -0.5f, "New optics changed historical FOV");

	auto rotated = Quad();
	rotated.worldPose.pose = Compose(tracking.head.pose, Quad().worldPose.pose);
	const auto latest = session.ViewsAt({Duration{2000}});
	const auto moved = PreparePreviewQuad(rotated, latest, {64, 32});
	Require(moved && moved->eyes[0], "Rotated head-relative quad");
	Near(moved->eyes[0]->viewCorners[0].x, -0.9f, "Rotated eye offset not applied in head frame");
	Near(moved->eyes[0]->viewCorners[0].y, 1, "World-to-view orientation mismatch");
	Near(moved->eyes[0]->viewCorners[0].z, -2, "Rotated view depth mismatch");
}

void VisibilityAndClipping()
{
	auto quad = Quad();
	auto views = Views();
	views.views.push_back({{0, ViewRole::Inset, 50, 50}, {IdentityPose(), TrackedPoseFlags}, Fov(-0.1f, 0.1f)});
	quad.eyes = XR_EYE_VISIBILITY_RIGHT;
	auto geometry = PreparePreviewQuad(quad, views, {64, 32});
	Require(geometry && !geometry->eyes[0] && geometry->eyes[1], "Per-eye visibility lost");
	Near(geometry->eyes[1]->tangents[0], -1, "Inset view replaced outer target");
	quad.eyes = XR_EYE_VISIBILITY_BOTH;
	quad.worldPose.flags = 0;
	Require(!PreparePreviewQuad(quad, views, {64, 32}), "Invalid space pose fell back to identity");
	quad = Quad();
	views.views[0].pose.flags = 0;
	geometry = PreparePreviewQuad(quad, views, {64, 32});
	Require(geometry && !geometry->eyes[0] && geometry->eyes[1], "Unavailable eye pose was drawn");
	views = Views();
	quad.worldPose.pose.position.z = 2;
	geometry = PreparePreviewQuad(quad, views, {64, 32});
	Require(geometry && !PreviewQuadBounds(*geometry->eyes[0], {0, 0, 100, 100}, {100, 100}),
			"Behind-eye quad produced diagnostic pixels");
	quad.worldPose.pose.position.z = -0.2f;
	quad.worldPose.pose.orientation = EulerAngles(1.2f, 0, 0);
	geometry = PreparePreviewQuad(quad, views, {64, 32});
	Require(geometry && geometry->eyes[0], "Near-plane crossing quad");
	const auto bounds = PreviewQuadBounds(*geometry->eyes[0], {0, 0, 100, 100}, {100, 100});
	Require(bounds && bounds->offset.x >= 0 && bounds->offset.y >= 0 &&
				bounds->offset.x + bounds->extent.width <= 100 && bounds->offset.y + bounds->extent.height <= 100,
			"Near-plane clipping did not bound diagnostic rectangle");
	for (const auto& corner : geometry->eyes[0]->stripClipCorners)
		for (float component : corner)
			Require(std::isfinite(component), "Near-plane geometry divided by depth");
	quad = Quad();
	quad.worldPose.pose.position.z = -0.000001f;
	geometry = PreparePreviewQuad(quad, views, {64, 32});
	const auto closeBounds = PreviewQuadBounds(*geometry->eyes[0], {0, 0, 100, 100}, {100, 100});
	Require(closeBounds && closeBounds->offset.x == 0 && closeBounds->offset.y == 0 &&
				closeBounds->extent.width == 100 && closeBounds->extent.height == 100,
			"A visible quad near the eye was clipped by an invented physical near plane");
	quad.color.rectangle.offset.x = 50;
	Require(!PreparePreviewQuad(quad, views, {64, 32}), "Invalid crop was silently clamped");
	quad = Quad();
	quad.size.width = 0;
	Require(!PreparePreviewQuad(quad, views, {64, 32}), "Empty quad generated geometry");
}

void LargeFiniteClipDistances()
{
	for (const float scale : {1.0f, std::numeric_limits<float>::max() * 0.4f})
	{
		PreviewQuadEye geometry;
		geometry.tangents = {-0.5f, 0.5f, 0.5f, -0.5f};
		geometry.viewCorners = {
			{{-scale, scale, -scale}, {scale, scale, -scale}, {scale, -scale, -scale}, {-scale, -scale, -scale}}};
		const auto bounds = PreviewQuadBounds(geometry, {0, 0, 100, 100}, {100, 100});
		Require(bounds && bounds->offset.x == 0 && bounds->offset.y == 0 && bounds->extent.width == 100 &&
					bounds->extent.height == 100,
				"Finite clip distances lost the visible viewport under uniform scaling");
	}
}

void ProjectionPreparation()
{
	ProjectionLayerDescription submitted;
	ProjectionViewDescription left{};
	left.color = {reinterpret_cast<XrSwapchain>(10), {{16, 8}, {32, 16}}, 3};
	ProjectionViewDescription right{};
	right.color = {reinterpret_cast<XrSwapchain>(11), {{4, 20}, {40, 80}}, 7};
	submitted.views = {left, right, left, right};
	unsigned resolutions = 0;
	const CompositionExtentResolver extent = [&](XrSwapchain source) -> std::optional<XrExtent2Di>
	{
		++resolutions;
		if (source == left.color.source)
			return XrExtent2Di{128, 64};
		if (source == right.color.source)
			return XrExtent2Di{64, 128};
		return {};
	};
	FrameLayers accepted({Duration{1000}}, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, {submitted});
	submitted.views[0].color.rectangle = {{0, 0}, {128, 64}};
	const auto& copied = std::get<ProjectionLayerDescription>(accepted.layers[0]);
	auto projection = PreparePreviewProjection(copied, extent);
	Require(projection && projection->eyes[0] && projection->eyes[1] && resolutions == 2,
			"Projection preview must prepare only the two accepted outer source views");
	Require(projection->displayExtent.width == 40 && projection->displayExtent.height == 80 &&
				projection->eyes[0]->image.arraySlice == 3 && projection->eyes[1]->image.arraySlice == 7,
			"Asymmetric eye dimensions or accepted array slices changed");
	Near(projection->eyes[0]->uvOffset[0], 0.125f, "Projection crop offset ignored");
	Near(projection->eyes[0]->uvScale[0], 0.25f, "Caller mutation changed accepted projection crop");
	const auto constants = PreviewProjectionConstants(*projection->eyes[0], {0, 0, 64, 32}, true);
	Require(constants.has_value(), "Projection blit constants unavailable");
	Near((*constants)[0], 0.125f, "Blit constant offset ABI");
	Near((*constants)[2], 0.25f, "Blit constant scale ABI");
	Near((*constants)[4], 0.25f * 0.25f / 64, "Blit downsample tap ABI");
	Near((*constants)[6], 1, "HDR sRGB encode option ABI");
	Require(!PreviewProjectionConstants(*projection->eyes[0], {0, 0, 0, 32}, false),
			"Empty destination generated invalid sample taps");
	const auto full = PreparePreviewProjection(copied, extent, PreviewCropSelection::WholeImage);
	Require(full && full->displayExtent.width == 128 && full->displayExtent.height == 128,
			"Explicit whole-image preview lost source dimensions");
	Near(full->eyes[0]->uvOffset[0], 0, "Whole-image debug override retained source offset");
	Near(full->eyes[0]->uvScale[0], 1, "Whole-image debug override retained source scale");

	const auto layout =
		PreparePreviewEyeLayout({10, 20, 101, 80}, PreviewEyeSelection::Both, PreviewEyePacking::Anaglyph);
	const auto draws = PreparePreviewProjectionDraws(*projection, layout, PreviewEyePacking::Anaglyph);
	Require(draws.count == 2 && draws.draws[0].sourceEye == 0 && draws.draws[1].sourceEye == 1 &&
				draws.draws[0].channels == PreviewColorChannels::Red &&
				draws.draws[1].channels == PreviewColorChannels::Cyan,
			"Anaglyph source order or channel selection changed");
	Near(draws.draws[1].target.rectangle.width, 101, "Anaglyph projection must occupy full fitted viewport");
	const auto rightOnly =
		PreparePreviewEyeLayout({-5, 0, 100, 80}, PreviewEyeSelection::Right, PreviewEyePacking::Anaglyph);
	const auto one = PreparePreviewProjectionDraws(*projection, rightOnly, PreviewEyePacking::Anaglyph);
	Require(one.count == 1 && one.draws[0].sourceEye == 1 && one.draws[0].channels == PreviewColorChannels::All,
			"Single-eye preview inherited anaglyph channel masking");

	auto empty = copied;
	empty.views[1].color.rectangle.extent.width = 0;
	projection = PreparePreviewProjection(empty, extent);
	Require(projection && !projection->eyes[1] &&
				PreparePreviewProjectionDraws(*projection, rightOnly, PreviewEyePacking::Anaglyph).count == 0,
			"Empty submitted eye silently substituted the other eye or entire image");
	empty.views[0].color.rectangle.extent.height = 0;
	projection = PreparePreviewProjection(empty, extent);
	Require(projection && projection->displayExtent.width == 0 && projection->displayExtent.height == 0,
			"Empty projection did not describe a clear preview");
	empty = copied;
	empty.views[0].color.rectangle.offset.x = 120;
	Require(!PreparePreviewProjection(empty, extent), "Invalid projection crop was silently clamped");
	empty.views[0].color = {reinterpret_cast<XrSwapchain>(999), {{0, 0}, {1, 1}}, 0};
	Require(!PreparePreviewProjection(empty, extent), "Unknown source replaced accepted projection identity");
}

void EyeLayout()
{
	const PreviewRectangle present{-10, 20, 101, 80};
	auto layout = PreparePreviewEyeLayout(present, PreviewEyeSelection::Both, PreviewEyePacking::SideBySide);
	Require(layout.count == 2 && layout.eyes[1].eye == 1, "Stereo eye order");
	Near(layout.eyes[1].rectangle.x, 40.5f, "Odd-width layout lost fractional eye partition");
	Near(layout.eyes[0].rectangle.width, 50.5f, "Stereo eye width");
	layout = PreparePreviewEyeLayout(present, PreviewEyeSelection::Both, PreviewEyePacking::SideBySide,
									 PreviewPixelAlignment::WholePixels);
	Near(layout.eyes[0].rectangle.width, 50, "D3D12 whole-pixel eye partition changed");
	Near(layout.eyes[1].rectangle.x, 40, "D3D12 whole-pixel right-eye origin changed");
	layout = PreparePreviewEyeLayout(present, PreviewEyeSelection::Both, PreviewEyePacking::OverUnder);
	Near(layout.eyes[1].rectangle.y, 60, "Over-under bottom eye");
	layout = PreparePreviewEyeLayout(present, PreviewEyeSelection::Both, PreviewEyePacking::Anaglyph);
	Near(layout.eyes[1].rectangle.x, -10, "Anaglyph eyes must share viewport");
	layout = PreparePreviewEyeLayout(present, PreviewEyeSelection::Right, PreviewEyePacking::SideBySide);
	Require(layout.count == 1 && layout.eyes[0].eye == 1, "Right-only eye selection");
	Near(layout.eyes[0].rectangle.width, 101, "Single eye must occupy full fitted viewport");
}
} // namespace

int main()
{
	try
	{
		GeometryAndShaderAbi();
		HistoricalSubmission();
		VisibilityAndClipping();
		LargeFiniteClipDistances();
		EyeLayout();
		ProjectionPreparation();
		std::cout << "Preview geometry tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
