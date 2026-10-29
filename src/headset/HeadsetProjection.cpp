#include "headset/HeadsetProjection.h"
#include "Pch.h"

namespace simxr::headset
{
XrResult PrepareHeadsetProjection(std::span<const FrameView> views, XrExtent2Di sourceExtent, XrExtent2Di outputExtent,
								  const std::array<XrFovf, 2>& fovs, CompositionPlan& plan)
{
	if (views.size() != 2 || sourceExtent.width <= 0 || sourceExtent.height <= 0 || outputExtent.width <= 0 ||
		outputExtent.height <= 0)
		return XR_ERROR_VALIDATION_FAILURE;
	CompositionTarget target{};
	target.width = static_cast<std::uint32_t>(outputExtent.width);
	target.height = static_cast<std::uint32_t>(outputExtent.height);
	ProjectionLayerDescription projection;
	const auto image = reinterpret_cast<XrSwapchain>(std::uintptr_t{1});
	for (std::size_t eye = 0; eye < views.size(); ++eye)
	{
		const PoseSample pose{views[eye].pose, ValidPoseFlags};
		target.eyes[eye] = {pose, fovs[eye]};
		ProjectionViewDescription view{};
		view.worldPose = pose;
		view.poseInSpace = views[eye].pose;
		view.fov = views[eye].fov;
		view.color = {image, {{0, 0}, sourceExtent}, static_cast<std::uint32_t>(eye)};
		projection.views.push_back(view);
	}
	// Composition uses only geometry here; this positive sentinel is never published as a frame time.
	const FrameLayers frame{{Duration{1}}, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, {std::move(projection)}};
	return PrepareComposition(
		frame, target,
		[=](XrSwapchain source) -> std::optional<XrExtent2Di> {
			return source == image ? std::optional{sourceExtent} : std::nullopt;
		},
		plan);
}
} // namespace simxr::headset
