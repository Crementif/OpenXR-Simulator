#pragma once

#include "graphics/Compositor.h"

namespace simxr
{
// Recompose individual retained layers using a newer head orientation. The
// submitted head position, eye offsets, FOVs and output dimensions stay fixed.
// VIEW offsets follow that rotation; LOCAL/STAGE layers keep their world poses.
// Other dynamic spaces require a different policy and return unsupported.
// This prepares geometry only: the caller owns images and display-time admission.
XrResult PrepareRotationalComposition(const FrameLayers& layers, const CompositionTarget& submittedTarget,
									  const PoseSample& submittedHead, const PoseSample& displayHead,
									  const CompositionExtentResolver& resolve, CompositionPlan& output);
} // namespace simxr
