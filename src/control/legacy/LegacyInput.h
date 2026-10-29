#pragma once

#include "simulation/InputState.h"
#include "simulation/TrackingTypes.h"
#include "simulation/ViewConfiguration.h"

#include <array>
#include <cstddef>

namespace simxr
{
class LegacyInput
{
public:
	LegacyInput();
	TrackingState Tracking(bool focused = true) const;
	std::array<ControllerControls, 2> Controls() const;
	const ViewSettings& Views() const;
	void SetViews(ViewSettings settings);

private:
	struct Controller
	{
		XrPosef offset;
		ControllerControls controls;
	};

	XrPosef m_head{{0, 0, 0, 1}, {0, 1.7f, 0}};
	std::array<Controller, 2> m_controllers;
	ViewSettings m_views;
};
} // namespace simxr
