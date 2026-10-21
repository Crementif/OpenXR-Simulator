#include "../TestSupport.h"
#include "Math.h"
#include "graphics/Compositor.h"
#include "runtime/Session.h"
#include "runtime/SimulationExtensions.h"

#include <array>
#include <cmath>
#include <iostream>

namespace
{
using namespace simxr;

void EnabledLayout()
{
	const std::array enabled{XR_VARJO_QUAD_VIEWS_EXTENSION_NAME, XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME};
	Instance instance(reinterpret_cast<XrInstance>(401), enabled, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(402), instance);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	Require(session.Ready(timing.epoch) == XR_SUCCESS, "Ready quad session");
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO};
	Require(session.Begin(begin, timing) == XR_SUCCESS && session.ActiveViewCount() == 4 &&
				session.ActiveViewConfiguration() == XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO,
			"Begin quad layout");
	FramePrediction prediction;
	Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS, "Issue quad frame");
	TrackingState tracking;
	tracking.head.pose = IdentityPose();
	tracking.head.pose.position = {1, 2, 3};
	tracking.head.flags = TrackedPoseFlags;
	ViewSettings settings;
	settings.outerFov[0] = {-0.45f, 1.05f, 0.8f, -0.7f};
	settings.outerFov[1] = {-1.05f, 0.45f, 0.7f, -0.8f};
	session.PublishTracking(prediction.displayTime, tracking, settings, prediction.id);
	XrSpace stage;
	Require(session.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, IdentityPose(), stage) == XR_SUCCESS,
			"Create quad reference space");
	XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO,
							prediction.displayTime.value.count(), stage};
	XrViewState state{XR_TYPE_VIEW_STATE};
	std::array<XrView, 4> views;
	for (auto& view : views)
		view = {XR_TYPE_VIEW};
	std::uint32_t count = 0;
	Require(session.LocateViews(locate, &state, 0, &count, nullptr) == XR_SUCCESS && count == 4, "Quad count query");
	views[0].pose.position.x = 123;
	Require(session.LocateViews(locate, &state, 3, &count, views.data()) == XR_ERROR_SIZE_INSUFFICIENT &&
				views[0].pose.position.x == 123,
			"Quad capacity failure mutated output");
	Require(session.LocateViews(locate, &state, 4, &count, views.data()) == XR_SUCCESS && count == 4,
			"Locate four historical views");
	for (std::size_t eye = 0; eye < 2; ++eye)
	{
		Require(views[eye].pose.position.x == views[eye + 2].pose.position.x &&
					views[eye].pose.position.y == views[eye + 2].pose.position.y &&
					views[eye].pose.position.z == views[eye + 2].pose.position.z &&
					views[eye].pose.orientation.w == views[eye + 2].pose.orientation.w,
				"VARJO revision2 outer/inset poses must match");
		Require(views[eye + 2].fov.angleLeft >= views[eye].fov.angleLeft &&
					views[eye + 2].fov.angleRight <= views[eye].fov.angleRight &&
					views[eye + 2].fov.angleUp <= views[eye].fov.angleUp &&
					views[eye + 2].fov.angleDown >= views[eye].fov.angleDown,
				"Inset escapes asymmetric outer field of view");
	}
	auto inactive = locate;
	inactive.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	Require(session.LocateViews(inactive, &state, 0, &count, nullptr) == XR_ERROR_VALIDATION_FAILURE,
			"Locate inactive supported configuration accepted");

	const auto fixedInset = views[2].fov;
	tracking.gaze.yaw = 0.3f;
	session.PublishTracking(prediction.displayTime, tracking, settings, prediction.id);
	XrSpace combined;
	Require(session.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO, IdentityPose(), combined) == XR_SUCCESS,
		"Create enabled rendering-gaze space");
	SpaceLocation gaze;
	Require(session.Spaces().Locate(combined, stage, prediction.displayTime, gaze) == XR_SUCCESS &&
		(gaze.pose.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT), "Rendering gaze availability");
	XrViewLocateFoveatedRenderingVARJO foveated{XR_TYPE_VIEW_LOCATE_FOVEATED_RENDERING_VARJO, nullptr, XR_TRUE};
	locate.next = &foveated;
	Require(session.LocateViews(locate, &state, 4, &count, views.data()) == XR_SUCCESS &&
		views[2].fov.angleLeft < fixedInset.angleLeft, "Timestamped gaze must move the inset FOV");
	foveated.foveatedRenderingActive = 2;
	Require(session.LocateViews(locate, &state, 4, &count, views.data()) == XR_ERROR_VALIDATION_FAILURE,
		"Invalid application foveation boolean accepted");
	foveated.foveatedRenderingActive = XR_TRUE;
	foveated.next = &foveated;
	Require(session.LocateViews(locate, &state, 4, &count, views.data()) == XR_ERROR_VALIDATION_FAILURE,
		"Cyclic foveation chain accepted");
	foveated.next = nullptr;

	std::array<XrCompositionLayerProjectionView, 4> submitted;
	const auto image = reinterpret_cast<XrSwapchain>(403);
	for (std::size_t index = 0; index < submitted.size(); ++index)
		submitted[index] = {XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW,
							nullptr,
							views[index].pose,
							views[index].fov,
							{image, {{0, 0}, {64, 48}}, static_cast<std::uint32_t>(index)}};
	XrCompositionLayerProjection projection{
		XR_TYPE_COMPOSITION_LAYER_PROJECTION, nullptr, 0, stage, 4, submitted.data()};
	const XrCompositionLayerBaseHeader* layer = reinterpret_cast<const XrCompositionLayerBaseHeader*>(&projection);
	XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,			 nullptr, locate.displayTime,
					   XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		  &layer};
	LayerValidationContext validation;
	validation.viewCount = session.ActiveViewCount();
	validation.validSpace = [&](XrSpace value) { return session.Spaces().Owns(value); };
	validation.swapchain = [&](XrSwapchain value) -> std::optional<SwapchainDescription>
	{ return value == image ? std::optional{SwapchainDescription{64, 48, 4, true}} : std::nullopt; };
	std::shared_ptr<const FrameLayers> prepared;
	auto resolve = [&](XrSpace space, RuntimeTime time, SpaceSample& pose)
	{ return session.Spaces().Sample(space, time, pose); };
	projection.viewCount = 2;
	Require(BuildLayers(end, validation, resolve, prepared) == XR_ERROR_LAYER_INVALID &&
				session.Scheduler().Snapshot().begun->id == prediction.id,
			"Two-view projection consumed quad frame");
	projection.viewCount = 4;
	Require(BuildLayers(end, validation, resolve, prepared) == XR_SUCCESS, "Prepare quad metadata");
	const auto world = session.ViewsAt(prediction.displayTime);
	Require(world.views.size() == 4 && world.views[2].description.role == ViewRole::Inset && !world.gazeActive,
			"Fixed inset layout unexpectedly uses gaze");
	CompositionTarget target{
		64, 48, {{{world.views[0].pose, world.views[0].fov}, {world.views[1].pose, world.views[1].fov}}}};
	CompositionPlan plan;
	Require(PrepareComposition(
				*prepared, target, [&](XrSwapchain) { return std::optional{XrExtent2Di{64, 48}}; }, plan) ==
					XR_SUCCESS &&
				plan.draws.size() == 4,
			"Quad composition plan");
	for (std::size_t index = 0; index < plan.draws.size(); ++index)
		Require(plan.draws[index].eye == index % 2 && plan.draws[index].source.arraySlice == index,
				"Outer/inset ordering or eye identity changed");
	Require(FindLayerImage(*prepared, 0, 3)->arraySlice == 3 && !FindLayerImage(*prepared, 0, 4),
			"Selected inset raw capture substituted an outer source");
	Require(session.EndFrame(prediction, &end, prepared) == XR_SUCCESS, "Submit four-view CPU metadata");
	const auto retained = session.History().Find({session.Snapshot().generation, prediction.id});
	Require(retained && retained->submission && retained->submission->viewLayout == ViewLayout::StereoWithInset &&
				std::get<ProjectionLayerDescription>(retained->submission->layers->layers[0]).views.size() == 4,
			"Historical submission lost inset metadata");
	Require(retained->submission->viewQuery && retained->submission->viewQuery->gazeActive &&
		retained->submission->viewQuery->foveatedRequested &&
		retained->submission->viewQuery->displayTime == prediction.displayTime,
		"Submitted frame lost its timestamp-matched foveation query");
	foveated.foveatedRenderingActive = XR_FALSE;
	Require(session.LocateViews(locate, &state, 4, &count, views.data()) == XR_SUCCESS &&
		views[2].fov.angleLeft == fixedInset.angleLeft && retained->submission->viewQuery->gazeActive,
		"Per-call fixed request changed retained foveation evidence");
	FramePrediction next;
	Require(session.WaitFrame(next) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS, "Issue gaze-loss frame");
	tracking.gaze.available = false;
	session.PublishTracking(next.displayTime, tracking, settings, next.id);
	foveated.foveatedRenderingActive = XR_TRUE;
	locate.displayTime = next.displayTime.value.count();
	Require(session.LocateViews(locate, &state, 4, &count, views.data()) == XR_SUCCESS &&
		views[2].fov.angleLeft == fixedInset.angleLeft, "Unavailable gaze must fall back to fixed insets");
	Require(session.Spaces().Locate(combined, stage, next.displayTime, gaze) == XR_SUCCESS &&
		!(gaze.pose.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT), "Unavailable rendering gaze remains tracked");
	locate.displayTime = prediction.displayTime.value.count();
	Require(session.LocateViews(locate, &state, 4, &count, views.data()) == XR_SUCCESS &&
		views[2].fov.angleLeft < fixedInset.angleLeft &&
		session.Spaces().Locate(combined, stage, prediction.displayTime, gaze) == XR_SUCCESS &&
		(gaze.pose.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT),
		"Historical query substituted the latest unavailable gaze");
	Require(session.EndFrame(next) == XR_SUCCESS, "Submit gaze-loss frame");
	Require(session.RequestExit(next.displayTime) == XR_SUCCESS &&
				session.End(prediction.displayTime) == XR_SUCCESS &&
				session.Ready(prediction.displayTime) == XR_SUCCESS,
			"Stop quad layout");
	begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	Require(session.Begin(begin, timing) == XR_SUCCESS && session.ActiveViewCount() == 2,
			"Restart retained quad layout");
	Require(retained->submission->viewLayout == ViewLayout::StereoWithInset, "Restart changed retained active layout");
}
} // namespace

int main()
{
	using namespace simxr;
	const auto plain = SimulationExtensions();
	const auto linux = SimulationExtensions({true});
	Require(linux.size() == plain.size() + 1 && linux.back().name == XR_VARJO_QUAD_VIEWS_EXTENSION_NAME &&
				linux.back().version == XR_VARJO_quad_views_SPEC_VERSION,
			"Quad platform capability metadata");
	Instance disabled(reinterpret_cast<XrInstance>(404), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(405), disabled);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	Require(session.Ready(timing.epoch) == XR_SUCCESS, "Ready extension-disabled session");
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO};
	Require(session.Begin(begin, timing) == XR_ERROR_VIEW_CONFIGURATION_TYPE_UNSUPPORTED && !session.Snapshot().running,
			"Quad configuration usable without enabled extension");
	XrSpace unsupported;
	Require(session.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO, IdentityPose(), unsupported) ==
		XR_ERROR_REFERENCE_SPACE_UNSUPPORTED, "Disabled combined-eye reference space accepted");
	const auto foveated = SimulationExtensions({true, true});
	ExtensionRegistry registry(foveated);
	const std::array<const char*, 1> missingDependency{XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME};
	Require(registry.Validate(missingDependency, XR_MAKE_VERSION(1, 0, 27)) == XR_ERROR_EXTENSION_NOT_PRESENT,
		"Foveation quad dependency missing");
	const std::array<const char*, 2> both{XR_VARJO_QUAD_VIEWS_EXTENSION_NAME, XR_VARJO_FOVEATED_RENDERING_EXTENSION_NAME};
	Require(registry.Validate(both, XR_MAKE_VERSION(1, 0, 27)) == XR_SUCCESS, "Select foveation extension");
	EnabledExtensions selected(both);
	XrSystemFoveatedRenderingPropertiesVARJO capability{XR_TYPE_SYSTEM_FOVEATED_RENDERING_PROPERTIES_VARJO};
	XrSystemProperties properties{XR_TYPE_SYSTEM_PROPERTIES, &capability};
	Require(FillSimulationProperties(selected, &properties) == XR_SUCCESS && capability.supportsFoveatedRendering,
		"Foveation capability missing");
	capability.supportsFoveatedRendering = 42;
	Require(FillSimulationProperties(EnabledExtensions{}, &properties) == XR_SUCCESS &&
		capability.supportsFoveatedRendering == 42, "Disabled capability output was modified");
	EnabledLayout();
	std::cout << "PASS: enabled quad layout, historical views, ordered composition and selected inset identities\n";
}
