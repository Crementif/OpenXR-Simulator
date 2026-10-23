#pragma once

#include "graphics/Compositor.h"

namespace simxr
{
class Session;
using SwapchainDescriptionResolver = std::function<std::optional<SwapchainDescription>(XrSwapchain)>;
struct SessionComposition
{
	std::shared_ptr<const FrameLayers> layers;
	CompositionTarget target{};
	PoseSample head;
	CompositionPlan plan;
};
XrResult PrepareSessionComposition(Session& session, const XrFrameEndInfo& info,
	const SwapchainDescriptionResolver& describe, SessionComposition& output);
}
