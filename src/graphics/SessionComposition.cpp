#include "graphics/SessionComposition.h"
#include "Pch.h"
#include "runtime/Session.h"
#include "runtime/validation/LayerValidation.h"

namespace simxr
{
XrResult PrepareSessionComposition(Session& session, const XrFrameEndInfo& info,
								   const SwapchainDescriptionResolver& describe, SessionComposition& output)
{
	LayerValidationContext validation;
	validation.viewCount = session.ActiveViewCount();
	validation.depthEnabled = session.Extensions().Contains(XR_KHR_COMPOSITION_LAYER_DEPTH_EXTENSION_NAME);
	validation.cylinderEnabled = session.Extensions().Contains(XR_KHR_COMPOSITION_LAYER_CYLINDER_EXTENSION_NAME);
	validation.validSpace = [&session](XrSpace space) {
		return session.Spaces().Owns(space);
	};
	validation.swapchain = describe;
	SessionComposition prepared;
	auto result = BuildLayers(
		info, validation,
		[&session](XrSpace space, RuntimeTime time, SpaceSample& pose) {
			return session.Spaces().Sample(space, time, pose);
		},
		prepared.layers);
	if (XR_FAILED(result))
		return result;
	if (info.layerCount)
	{
		const auto views = session.ViewsAt({Duration{info.displayTime}});
		if (views.views.size() < 2)
			return XR_ERROR_RUNTIME_FAILURE;
		prepared.head = views.head;
		prepared.target = {views.views[0].description.width,
						   views.views[0].description.height,
						   {{{views.views[0].pose, views.views[0].fov}, {views.views[1].pose, views.views[1].fov}}}};
		result = PrepareComposition(
			*prepared.layers, prepared.target,
			[&describe](XrSwapchain swapchain) -> std::optional<XrExtent2Di> {
				const auto description = describe(swapchain);
				if (!description)
					return {};
				constexpr auto maxExtent = static_cast<std::uint32_t>(std::numeric_limits<std::int32_t>::max());
				if (description->width > maxExtent || description->height > maxExtent)
					return XrExtent2Di{};
				return XrExtent2Di{static_cast<std::int32_t>(description->width),
								   static_cast<std::int32_t>(description->height)};
			},
			prepared.plan);
		if (XR_FAILED(result))
			return result;
	}
	output = std::move(prepared);
	return XR_SUCCESS;
}
} // namespace simxr
