#include "simulation/LegacyControllerMotion.h"
#include "Pch.h"

#include "math/Math.h"

namespace simxr::legacy
{
template <typename T> static void Merge(std::optional<T>& current, const ControllerOverrideEdit<T>& edit)
{
	switch (edit.change)
	{
	case OverrideChange::Keep:
		break;
	case OverrideChange::Set:
		current = edit.value;
		break;
	case OverrideChange::Release:
		current.reset();
		break;
	}
}
static float Coordinate(double value)
{
	if (std::abs(value) > std::numeric_limits<float>::max())
		return std::copysign(std::numeric_limits<float>::infinity(), value);
	return static_cast<float>(value);
}

LegacyControllerMotion::LegacyControllerMotion()
{
	m_controllers[0].local = {{-0.2f, -0.3f, -0.4f}, 0, std::numbers::pi_v<float> / 2, 0};
	m_controllers[1].local = {{0.2f, -0.3f, -0.4f}, 0, std::numbers::pi_v<float> / 2, 0};
}
std::size_t LegacyControllerMotion::HandIndex(ControllerHand hand)
{
	switch (hand)
	{
	case ControllerHand::Left:
		return 0;
	case ControllerHand::Right:
		return 1;
	}
	throw std::invalid_argument("Unknown legacy controller hand");
}
XrPosef LegacyControllerMotion::WorldPose(ControllerLocalPose local, ControllerHeadPose head)
{
	const auto cosine = std::cos(static_cast<double>(head.yaw)), sine = std::sin(static_cast<double>(head.yaw));
	const auto yaw = (static_cast<double>(head.yaw) + local.yaw) / 2;
	const auto pitch = (static_cast<double>(head.pitch) + local.pitch) / 2;
	const XrQuaternionf yawRotation{0, static_cast<float>(std::sin(yaw)), 0, static_cast<float>(std::cos(yaw))};
	const XrQuaternionf pitchRotation{static_cast<float>(std::sin(pitch)), 0, 0, static_cast<float>(std::cos(pitch))};
	return {Multiply(Multiply(yawRotation, pitchRotation), EulerAngles(0, 0, local.roll)),
			{Coordinate(head.position.x + local.position.x * cosine + local.position.z * sine),
			 Coordinate(static_cast<double>(head.position.y) + local.position.y),
			 Coordinate(head.position.z - local.position.x * sine + local.position.z * cosine)}};
}
void LegacyControllerMotion::Trigger(Controller& controller, float value)
{
	controller.controls.trigger = value;
	controller.controls.triggerTouch = value >= 0.5f;
}
void LegacyControllerMotion::Grip(Controller& controller, float value)
{
	controller.controls.grip = controller.controls.gripForce = value;
}
void LegacyControllerMotion::ThumbClick(Controller& controller, bool value)
{
	controller.controls.thumbClick = controller.controls.trackpadClick = value;
	controller.controls.thumbTouch = controller.controls.trackpadTouch = value;
}
void LegacyControllerMotion::Stick(Controller& controller, XrVector2f value)
{
	controller.controls.stick = controller.controls.trackpad = value;
}
void LegacyControllerMotion::AdvanceGlides(double seconds)
{
	for (std::size_t hand = 0; hand < m_glides.size(); ++hand)
	{
		auto& glide = m_glides[hand];
		if (!glide.active)
			continue;
		auto progress = static_cast<float>((seconds - glide.startSeconds) / glide.durationSeconds);
		if (progress >= 1)
		{
			progress = 1;
			glide.active = false;
		}
		auto& pose = m_controllers[hand].local;
		pose.position = {std::lerp(glide.from.position.x, glide.to.position.x, progress),
						 std::lerp(glide.from.position.y, glide.to.position.y, progress),
						 std::lerp(glide.from.position.z, glide.to.position.z, progress)};
		pose.yaw = std::lerp(glide.from.yaw, glide.to.yaw, progress);
		pose.pitch = std::lerp(glide.from.pitch, glide.to.pitch, progress);
	}
}
std::optional<bool> LegacyControllerMotion::ApplyKeyboard(const ControllerKeyboardSample& keyboard)
{
	if (!keyboard.active)
		return {};
	std::optional<bool> toggled;
	if (keyboard.animationKeyDown && !m_animationKeyDown)
	{
		m_animationEnabled = !m_animationEnabled;
		toggled = m_animationEnabled;
	}
	m_animationKeyDown = keyboard.animationKeyDown;
	auto& right = m_controllers[1].local;
	if (m_animationEnabled)
	{
		m_animationSeconds += keyboard.frameSeconds;
		constexpr float speed = 0.5f, radius = 0.4f;
		const auto phase = m_animationSeconds * speed;
		right.position = {radius * std::sin(phase * 2), -0.2f + 0.3f * std::sin(phase),
						  -0.4f + radius * std::sin(phase) * std::cos(phase)};
		right.yaw = 0.5f * std::sin(phase * 1.5f);
		right.pitch = -0.3f + 0.3f * std::cos(phase);
	}
	else
	{
		const auto distance = 0.5f * keyboard.frameSeconds;
		right.position.x += keyboard.translationDirection.x * distance;
		right.position.y += keyboard.translationDirection.y * distance;
		right.position.z += keyboard.translationDirection.z * distance;
		right.yaw += keyboard.rotationDirection.x * keyboard.frameSeconds;
		right.pitch += keyboard.rotationDirection.y * keyboard.frameSeconds;
	}
	if (keyboard.resetRight)
	{
		right.position = {0.2f, -0.3f, -0.4f};
		right.yaw = 0;
		right.pitch = std::numbers::pi_v<float> / 2;
		m_animationEnabled = false;
	}
	for (std::size_t hand = 0; hand < m_controllers.size(); ++hand)
	{
		auto& controller = m_controllers[hand];
		const auto& keys = keyboard.hands[hand];
		Trigger(controller, keys.trigger ? 1.0f : 0.0f);
		Grip(controller, keys.grip ? 1.0f : 0.0f);
		controller.controls.menu = keys.menu;
		controller.controls.primary = controller.controls.primaryTouch = keys.primary;
		controller.controls.secondary = controller.controls.secondaryTouch = keys.secondary;
		if (keys.thumbClick)
			ThumbClick(controller, *keys.thumbClick);
		Stick(controller, keys.stick);
	}
	return toggled;
}
void LegacyControllerMotion::ApplyPose(const ControllerPoseEdit& edit)
{
	const auto hand = HandIndex(edit.hand);
	auto& controller = m_controllers[hand];
	auto& glide = m_glides[hand];
	glide.active = edit.duration.count() > 0;
	if (glide.active)
	{
		glide.from = controller.local;
		glide.to = {edit.position, edit.yaw, edit.pitch, controller.local.roll};
		glide.startSeconds = edit.receivedSeconds;
		glide.durationSeconds = std::chrono::duration<double>(edit.duration).count();
	}
	else
	{
		controller.local.position = edit.position;
		controller.local.yaw = edit.yaw;
		controller.local.pitch = edit.pitch;
	}
	if (edit.trigger)
		Trigger(controller, *edit.trigger);
	if (edit.primary)
		controller.controls.primary = controller.controls.primaryTouch = *edit.primary;
}
void LegacyControllerMotion::ApplyHoldEdit(const ControllerHoldEdit& edit)
{
	for (std::size_t hand = 0; hand < m_holds.size(); ++hand)
	{
		if (edit.hand && HandIndex(*edit.hand) != hand)
			continue;
		auto& hold = m_holds[hand];
		if (edit.clear)
		{
			hold = {};
			continue;
		}
		Merge(hold.trigger, edit.trigger);
		Merge(hold.grip, edit.grip);
		Merge(hold.primary, edit.primary);
		Merge(hold.secondary, edit.secondary);
		Merge(hold.menu, edit.menu);
		Merge(hold.thumbClick, edit.thumbClick);
		Merge(hold.stickX, edit.stickX);
		Merge(hold.stickY, edit.stickY);
		Merge(hold.yaw, edit.yaw);
		Merge(hold.pitch, edit.pitch);
		Merge(hold.roll, edit.roll);
	}
}
void LegacyControllerMotion::ApplyWheel(double seconds)
{
	if (!m_wheel.enabled)
		return;
	const auto elapsed = seconds - m_wheel.receivedSeconds;
	m_wheelAngle =
		static_cast<float>(m_wheel.amplitude * std::sin(2 * std::numbers::pi * m_wheel.frequencyHz * elapsed));
	const auto cosine = std::cos(static_cast<double>(m_wheel.tilt)), sine = std::sin(static_cast<double>(m_wheel.tilt));
	for (std::size_t hand = 0; hand < m_controllers.size(); ++hand)
	{
		auto& controller = m_controllers[hand];
		const auto bearing = static_cast<double>(hand == 0 ? -m_wheel.spread : m_wheel.spread) + m_wheelAngle;
		const auto along = std::sin(bearing), up = std::cos(bearing);
		controller.local.position = {Coordinate(m_wheel.hub.x + m_wheel.radius * along),
									 Coordinate(m_wheel.hub.y + m_wheel.radius * up * cosine),
									 Coordinate(m_wheel.hub.z - m_wheel.radius * up * sine)};
		controller.local.yaw = 0;
		controller.local.pitch = -m_wheel.tilt;
		controller.local.roll = m_wheelAngle;
		Grip(controller, m_wheel.grip);
	}
}
void LegacyControllerMotion::ApplyHolds()
{
	for (std::size_t hand = 0; hand < m_holds.size(); ++hand)
	{
		const auto& hold = m_holds[hand];
		auto& controller = m_controllers[hand];
		if (hold.trigger)
			Trigger(controller, *hold.trigger);
		if (hold.grip)
			Grip(controller, *hold.grip);
		if (hold.primary)
			controller.controls.primary = controller.controls.primaryTouch = *hold.primary;
		if (hold.secondary)
			controller.controls.secondary = controller.controls.secondaryTouch = *hold.secondary;
		if (hold.menu)
			controller.controls.menu = *hold.menu;
		if (hold.thumbClick)
			ThumbClick(controller, *hold.thumbClick);
		if (hold.stickX)
			Stick(controller, {*hold.stickX, controller.controls.stick.y});
		if (hold.stickY)
			Stick(controller, {controller.controls.stick.x, *hold.stickY});
		if (hold.yaw)
			controller.local.yaw = *hold.yaw;
		if (hold.pitch)
			controller.local.pitch = *hold.pitch;
		if (hold.roll)
			controller.local.roll = *hold.roll;
	}
}
ControllerMotionResult LegacyControllerMotion::Update(const ControllerMotionFrame& frame)
{
	// Validate hand identities before mutating any channel. Frame input contains
	// no callbacks or native resources; evaluation and publication cannot allocate.
	if (frame.pose)
		HandIndex(frame.pose->hand);
	if (frame.hold && frame.hold->hand)
		HandIndex(*frame.hold->hand);
	std::lock_guard lock(m_mutex);
	AdvanceGlides(frame.glideSeconds);
	const auto toggled = ApplyKeyboard(frame.keyboard);
	if (frame.pose)
		ApplyPose(*frame.pose);
	if (frame.hold)
		ApplyHoldEdit(*frame.hold);
	if (frame.wheel)
		m_wheel = *frame.wheel;
	ApplyWheel(frame.wheelSeconds);
	ApplyHolds();
	m_head = frame.head;
	return {SnapshotLocked(m_head), toggled};
}
ControllerMotionSnapshot LegacyControllerMotion::SnapshotLocked(ControllerHeadPose head) const
{
	ControllerMotionSnapshot snapshot;
	snapshot.animationEnabled = m_animationEnabled;
	snapshot.wheelAngle = m_wheelAngle;
	for (std::size_t hand = 0; hand < m_controllers.size(); ++hand)
	{
		const auto& controller = m_controllers[hand];
		snapshot.hands[hand] = {controller.local, WorldPose(controller.local, head), controller.controls};
	}
	return snapshot;
}
ControllerMotionSnapshot LegacyControllerMotion::Snapshot() const
{
	std::lock_guard lock(m_mutex);
	return SnapshotLocked(m_head);
}
ControllerMotionSnapshot LegacyControllerMotion::Snapshot(ControllerHeadPose head) const
{
	std::lock_guard lock(m_mutex);
	return SnapshotLocked(head);
}
} // namespace simxr::legacy
