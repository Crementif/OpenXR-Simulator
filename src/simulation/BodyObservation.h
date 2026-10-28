#pragma once

#include "simulation/TrackingTypes.h"

namespace simxr
{
BodyObservation NormalizeBodyObservation(BodyObservation observation);
BodyObservation RebaseBodyObservation(const BodyObservation& observation, RuntimeTime origin, RuntimeTime target);
BodySample EvaluateBodyObservation(const BodyObservation& observation, bool focused);
} // namespace simxr
