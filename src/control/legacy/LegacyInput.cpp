#include "control/legacy/LegacyInput.h"
#include "Pch.h"

#include "math/Math.h"

namespace simxr
{
LegacyInput::LegacyInput()
{
	m_controllers[0].offset = {{0, 0, 0, 1}, {-0.2f, -0.3f, -0.4f}};
	m_controllers[1].offset = {{0, 0, 0, 1}, {0.2f, -0.3f, -0.4f}};
	m_views.outerWidth = m_views.outerHeight = 1280;
	m_views.outerFov.fill({-0.7853982f, 0.7853982f, 0.7853982f, -0.7853982f});
}

TrackingState LegacyInput::Tracking(bool focused) const
{
	TrackingState tracking;
	tracking.head.pose = m_head;
	tracking.focused = focused;
	for (std::size_t hand = 0; hand < m_controllers.size(); ++hand)
		tracking.controllers[hand].pose.pose = Compose(m_head, m_controllers[hand].offset);
	return tracking;
}

std::array<ControllerControls, 2> LegacyInput::Controls() const
{
	return {m_controllers[0].controls, m_controllers[1].controls};
}

const ViewSettings& LegacyInput::Views() const
{
	return m_views;
}

void LegacyInput::SetViews(ViewSettings settings)
{
	m_views = std::move(settings);
}
} // namespace simxr
