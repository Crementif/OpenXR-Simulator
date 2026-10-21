#include "../TestSupport.h"
#include "Math.h"
#include "graphics/LayerBuilder.h"
#include "runtime/Instance.h"
#include "runtime/Session.h"

#include <array>
#include <cmath>
#include <iostream>

namespace
{
using namespace simxr;

struct Fixture
{
	XrSwapchain image = reinterpret_cast<XrSwapchain>(1);
	XrSpace space = reinterpret_cast<XrSpace>(2);
	LayerValidationContext validation;
	std::array<XrCompositionLayerProjectionView, 4> views{};
	std::array<XrCompositionLayerDepthInfoKHR, 4> depths{};
	XrCompositionLayerProjection projection{XR_TYPE_COMPOSITION_LAYER_PROJECTION};
	XrCompositionLayerQuad quad{XR_TYPE_COMPOSITION_LAYER_QUAD};
	std::array<const XrCompositionLayerBaseHeader*, 2> layers{};
	XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO, nullptr, 1000, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 2, layers.data()};

	Fixture()
	{
		validation.validSpace = [this](XrSpace value) { return value == space; };
		validation.swapchain = [this](XrSwapchain value) -> std::optional<SwapchainDescription>
		{ return value == image ? std::optional{SwapchainDescription{128, 64, 4, true}} : std::nullopt; };
		validation.depthEnabled = true;
		for (std::size_t i = 0; i < views.size(); ++i)
		{
			auto& view = views[i];
			view.type = XR_TYPE_COMPOSITION_LAYER_PROJECTION_VIEW;
			view.pose = IdentityPose();
			view.pose.position.x = static_cast<float>(i + 1);
			view.fov = {-0.7f, 0.8f, 0.6f, -0.9f};
			view.subImage = {image, {{4, 8}, {64, 32}}, static_cast<std::uint32_t>(i)};
			depths[i] = {XR_TYPE_COMPOSITION_LAYER_DEPTH_INFO_KHR, nullptr, view.subImage, 0, 1, 0.1f, 100};
			view.next = &depths[i];
		}
		projection.space = space;
		projection.layerFlags = XR_COMPOSITION_LAYER_BLEND_TEXTURE_SOURCE_ALPHA_BIT;
		projection.viewCount = 2;
		projection.views = views.data();
		quad.space = space;
		quad.eyeVisibility = XR_EYE_VISIBILITY_LEFT;
		quad.pose = IdentityPose();
		quad.pose.position.z = -2;
		quad.size = {3, 2};
		quad.subImage = views[0].subImage;
		layers = {reinterpret_cast<XrCompositionLayerBaseHeader*>(&quad),
				  reinterpret_cast<XrCompositionLayerBaseHeader*>(&projection)};
	}
};

void CopiedMetadata()
{
	Fixture f;
	std::uint32_t resolutions = 0;
	LayerSpaceResolver resolve = [&](XrSpace space, RuntimeTime time, SpaceSample& result)
	{
		Require(space == f.space && time == RuntimeTime{Duration{1000}}, "Submission time changed during resolution");
		++resolutions;
		result.worldPose = {IdentityPose(), TrackedPoseFlags, time};
		const float half = std::sqrt(0.5f);
		result.worldPose.pose.orientation = {0, half, 0, half};
		result.worldPose.pose.position = {10, 1, 0};
		return XR_SUCCESS;
	};
	std::shared_ptr<const FrameLayers> prepared;
	Require(BuildLayers(f.info, f.validation, resolve, prepared) == XR_SUCCESS && resolutions == 2,
			"Mixed layer build failed");
	const auto& quad = std::get<QuadLayerDescription>(prepared->layers[0]);
	const auto& projection = std::get<ProjectionLayerDescription>(prepared->layers[1]);
	Require(quad.eyes == XR_EYE_VISIBILITY_LEFT && quad.size.width == 3 && quad.color.source == f.image &&
				std::abs(quad.worldPose.pose.position.x - 8) < 0.0001f,
			"Ordered quad metadata/world transform lost");
	Require(projection.space.source == f.space && projection.space.flags == f.projection.layerFlags &&
				projection.views.size() == 2 && projection.views[1].color.arraySlice == 1 &&
				projection.views[0].depth && projection.views[0].depth->nearZ == 0.1f &&
				projection.views[0].depth->farZ == 100 &&
				std::abs(projection.views[0].worldPose.pose.position.z + 1) < 0.0001f,
			"Projection metadata/world transform lost");
	f.views[0].pose.position.x = 100;
	f.views[0].fov.angleLeft = -1;
	f.views[0].subImage.imageRect.extent.width = 1;
	f.depths[0].farZ = 200;
	f.quad.size.width = 99;
	f.layers.fill(nullptr);
	Require(projection.views[0].poseInSpace.position.x == 1 && projection.views[0].fov.angleLeft == -0.7f &&
				projection.views[0].color.rectangle.extent.width == 64 && projection.views[0].depth->farZ == 100 &&
				quad.size.width == 3,
			"Prepared frame retained caller memory");
	const auto old = prepared;
	Require(BuildLayers(f.info, f.validation, resolve, prepared) == XR_ERROR_LAYER_INVALID && prepared == old &&
				resolutions == 2,
			"Invalid frame mutated output or resolved spaces before validation");
	f.info.layerCount = 0;
	Require(BuildLayers(f.info, f.validation, {}, prepared) == XR_SUCCESS && prepared->layers.empty(),
			"Explicit clear metadata failed");
}

void HistoricalAndFourViews()
{
	Fixture f;
	TrackingSystem tracking;
	TrackingState state;
	state.head.pose.position.x = 1;
	tracking.Publish({Duration{1000}}, state);
	state.head.pose.position.x = 9;
	tracking.Publish({Duration{2000}}, state);
	SpaceSystem spaces(reinterpret_cast<XrSession>(3), tracking);
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, IdentityPose(), f.space) == XR_SUCCESS,
			"Historical view space");
	f.quad.space = f.projection.space = f.space;
	f.validation.viewCount = f.projection.viewCount = 4;
	LayerSpaceResolver resolve = [&](XrSpace space, RuntimeTime time, SpaceSample& result)
	{ return spaces.Sample(space, time, result); };
	std::shared_ptr<const FrameLayers> prepared;
	Require(BuildLayers(f.info, f.validation, resolve, prepared) == XR_SUCCESS, "Four-view CPU metadata build");
	const auto& projection = std::get<ProjectionLayerDescription>(prepared->layers[1]);
	Require(projection.views.size() == 4 && projection.views[3].color.arraySlice == 3 &&
				projection.space.worldPose.sampleTime == RuntimeTime{Duration{1000}} &&
				projection.space.worldPose.pose.position.x == 1 && projection.views[0].worldPose.pose.position.x == 2,
			"Historical resolution used latest tracking instead of submitted time");
	for (std::uint32_t view = 0; view < 4; ++view)
	{
		const auto selected = FindLayerImage(*prepared, 1, view);
		Require(selected && selected->source == f.image && selected->arraySlice == view &&
					selected->rectangle.offset.x == 4 && selected->rectangle.offset.y == 8 &&
					selected->rectangle.extent.width == 64,
				"Layer selection lost outer/inset source crop or array slice");
	}
	const auto selectedQuad = FindLayerImage(*prepared, 0, 0);
	Require(selectedQuad && selectedQuad->arraySlice == 0 && !FindLayerImage(*prepared, 0, 1) &&
				!FindLayerImage(*prepared, 1, 4) && !FindLayerImage(*prepared, 2, 0),
			"Quad selection accepted a projection view or missing layer index");
	const auto old = prepared;
	Require(BuildLayers(
				f.info, f.validation, [](XrSpace, RuntimeTime, SpaceSample&) { return XR_ERROR_HANDLE_INVALID; },
				prepared) == XR_ERROR_HANDLE_INVALID &&
				prepared == old,
			"Resolution failure replaced prior metadata");
	Require(BuildLayers(
				f.info, f.validation,
				[](XrSpace, RuntimeTime time, SpaceSample& pose)
				{
					pose.worldPose = {IdentityPose(),
							XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT, time};
					return XR_SUCCESS;
				},
				prepared) == XR_SUCCESS &&
				!std::get<ProjectionLayerDescription>(prepared->layers[1]).views[0].worldPose.flags,
			"Translated pose became valid without source orientation");
}

void TransactionalSubmission()
{
	Fixture f;
	Instance instance(reinterpret_cast<XrInstance>(10), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(11), instance);
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	config.epoch = {Duration{1}};
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready({Duration{1}}) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Submission lifecycle");
	FramePrediction prediction;
	Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
			"Submission frame issue");
	const auto key = FrameKey{session.Snapshot().generation, prediction.id};
	std::shared_ptr<const FrameLayers> prepared;
	LayerSpaceResolver resolve = [](XrSpace, RuntimeTime time, SpaceSample& pose)
	{
		pose.worldPose = {IdentityPose(), TrackedPoseFlags, time};
		return XR_SUCCESS;
	};
	f.views[0].pose.orientation.w = 0;
	Require(BuildLayers(f.info, f.validation, resolve, prepared) == XR_ERROR_POSE_INVALID &&
				session.Scheduler().Snapshot().begun && session.History().Find(key)->stage == FrameStage::Begun,
			"Rejected preparation consumed frame");
	f.views[0].pose = IdentityPose();
	Require(BuildLayers(f.info, f.validation, resolve, prepared) == XR_SUCCESS, "Preparation retry");
	Require(session.EndFrame(prediction, &f.info) == XR_ERROR_VALIDATION_FAILURE,
			"Nonempty submission accepted without copied layers");
	++f.info.displayTime;
	Require(session.EndFrame(prediction, &f.info, prepared) == XR_ERROR_VALIDATION_FAILURE &&
				session.Scheduler().Snapshot().begun,
			"Mismatched metadata consumed frame");
	--f.info.displayTime;
	struct Output final : FrameOutput
	{
		CompletionStatus PollCompletion() const noexcept override
		{
			return CompletionStatus::Pending;
		}
	};
	const auto output = std::make_shared<Output>();
	std::size_t nativeCalls = 0;
	Require(session.EndFrame(prediction, &f.info, prepared, output,
							 [&]
							 {
								 ++nativeCalls;
								 return XR_ERROR_RUNTIME_FAILURE;
							 }) == XR_ERROR_RUNTIME_FAILURE &&
				nativeCalls == 1 && session.History().Find(key)->stage == FrameStage::Begun &&
				session.Scheduler().Snapshot().begun && session.Display().Snapshot().queued.empty(),
			"Native submit failure consumed frame or presentation reservation");
	Require(session.EndFrame(prediction, &f.info, prepared, output,
							 [&]
							 {
								 ++nativeCalls;
								 return XR_SUCCESS;
							 }) == XR_SUCCESS &&
				nativeCalls == 2 && session.Display().Snapshot().queued.size() == 1,
			"Prepared native submission retry failed");
	f.quad.size.width = 999;
	const auto record = session.History().Find(key);
	Require(record->stage == FrameStage::Submitted && record->submission->layers == prepared &&
				std::get<QuadLayerDescription>(record->submission->layers->layers[0]).size.width == 3,
			"History did not retain immutable copied metadata");
}
} // namespace

int main()
{
	CopiedMetadata();
	HistoricalAndFourViews();
	TransactionalSubmission();
	std::cout << "Layer builder tests passed\n";
}
