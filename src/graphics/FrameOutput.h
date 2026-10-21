#pragma once

#include "graphics/GpuCompletion.h"

namespace simxr
{
// A backend-owned private output and its producer completion observation.
// Polling must be thread-safe and nonblocking: no submissions, context changes,
// CPU waits, or mutation of native queue access. Complete permits selection;
// Complete and Failed are terminal observations.
// Failed does not establish GPU idleness. The backend independently preserves
// native resource safety after failure, cancellation, or the last CPU release.
class FrameOutput : public GpuCompletion
{
  public:
	~FrameOutput() override = default;
};
} // namespace simxr
