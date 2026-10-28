#pragma once

#include "graphics/Compositor.h"
#include <span>

namespace simxr::headset
{
// Map an already composed stereo image to another FOV without inventing new eye
// positions or orientations. The shared shader clears rays outside the source.
XrResult PrepareHeadsetProjection(std::span<const FrameView> views, XrExtent2Di sourceExtent, XrExtent2Di outputExtent,
								  const std::array<XrFovf, 2>& fovs, CompositionPlan& plan);
} // namespace simxr::headset
