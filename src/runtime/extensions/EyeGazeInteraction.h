#pragma once

#include "runtime/ExtensionRegistry.h"
#include "simulation/InputState.h"
#include "simulation/TrackingTypes.h"

namespace simxr
{
class EyeGazeInteraction
{
public:
	explicit EyeGazeInteraction(const EnabledExtensions& enabled, bool supported = true);
	bool Supports() const;
	void ContributeInput(const TrackingSnapshot& snapshot, InputSnapshot& input) const;
	PoseSample Pose(const TrackingSnapshot& snapshot) const;

private:
	const bool m_enabled;
	const bool m_supported;
};
} // namespace simxr
