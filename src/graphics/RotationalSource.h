#pragma once

#include "graphics/FrameOutput.h"
#include "simulation/TrackingTypes.h"

#include <memory>

namespace simxr
{
// Retained per-layer inputs are owned by presentation, separately from an
// image reader. Reproject may submit on an independently owned graphics queue.
class RotationalSource : public GpuCompletion
{
public:
	virtual PoseSample SubmittedHead() const noexcept = 0;
	// Success publishes a new output; timeout/capacity/failure leave it unchanged.
	// PollCompletion observes input readiness only and never submits GPU work.
	virtual XrResult Reproject(const PoseSample& head, std::shared_ptr<const FrameOutput>& output) const = 0;
};

struct ReprojectionInput
{
	std::shared_ptr<const RotationalSource> source;
	// Null/success means no retained source is attached. Null/failure records why
	// optional retention was unavailable while preserving the ordinary image.
	XrResult status = XR_SUCCESS;
};
} // namespace simxr
