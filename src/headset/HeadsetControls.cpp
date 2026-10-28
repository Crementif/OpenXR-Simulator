#include "Pch.h"
#include "headset/HeadsetActions.h"

#include <cmath>

namespace simxr::headset
{
bool NeutralHeadsetControls(const HeadsetHandSample& hand)
{
	const auto& controls = hand.controls;
	if ((hand.select.isActive && hand.select.currentState) ||
		(hand.trigger.isActive && !(std::abs(hand.trigger.currentState) <= .1f)) ||
		controls.triggerClick.value_or(false))
		return false;
	for (const bool pressed :
		 {controls.primary, controls.secondary, controls.menu, controls.back, controls.thumbClick,
		  controls.trackpadClick, controls.primaryTouch, controls.secondaryTouch, controls.triggerTouch,
		  controls.thumbTouch, controls.trackpadTouch, controls.thumbrestTouch})
		if (pressed)
			return false;
	for (const auto value : {controls.trigger, controls.grip, controls.gripForce, controls.trackpadForce,
							 controls.stick.x, controls.stick.y})
		if (!(std::abs(value) <= .1f))
			return false;
	return true;
}
} // namespace simxr::headset
