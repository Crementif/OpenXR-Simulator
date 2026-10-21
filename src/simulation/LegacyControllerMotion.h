#pragma once

#include "simulation/InputState.h"

#include <array>
#include <chrono>
#include <mutex>
#include <optional>

namespace simxr::legacy
{
enum class ControllerHand
{
	Left,
	Right
};
struct ControllerLocalPose
{
	XrVector3f position{};
	float yaw = 0;
	float pitch = 0;
	float roll = 0;
};
struct ControllerHeadPose
{
	XrVector3f position{};
	float yaw = 0;
	float pitch = 0;
};
struct ControllerKeyboardHand
{
	bool trigger = false;
	bool grip = false;
	bool menu = false;
	bool primary = false;
	bool secondary = false;
	// The legacy left hand has no keyboard thumb-click binding. Missing keeps
	// that channel unchanged instead of inventing a physical release.
	std::optional<bool> thumbClick;
	XrVector2f stick{};
};
struct ControllerKeyboardSample
{
	bool active = false;
	bool animationKeyDown = false;
	bool resetRight = false;
	float frameSeconds = 0;
	XrVector3f translationDirection{};
	XrVector2f rotationDirection{}; // yaw, pitch
	std::array<ControllerKeyboardHand, 2> hands;
};
struct ControllerPoseEdit
{
	ControllerHand hand = ControllerHand::Right;
	XrVector3f position{0.2f, -0.3f, -0.4f};
	float yaw = 0;
	float pitch = 1.5707963268f;
	std::chrono::duration<double, std::milli> duration{};
	double receivedSeconds = 0;
	std::optional<float> trigger;
	std::optional<bool> primary;
};
enum class OverrideChange
{
	Keep,
	Set,
	Release
};
template <typename T> struct ControllerOverrideEdit
{
	OverrideChange change = OverrideChange::Keep;
	T value{};
	static ControllerOverrideEdit Set(T value)
	{
		return {OverrideChange::Set, value};
	}
	static ControllerOverrideEdit Release()
	{
		return {OverrideChange::Release, {}};
	}
};
struct ControllerHoldEdit
{
	std::optional<ControllerHand> hand; // missing selects both hands
	bool clear = false;
	ControllerOverrideEdit<float> trigger;
	ControllerOverrideEdit<float> grip;
	ControllerOverrideEdit<float> stickX;
	ControllerOverrideEdit<float> stickY;
	ControllerOverrideEdit<float> yaw;
	ControllerOverrideEdit<float> pitch;
	ControllerOverrideEdit<float> roll;
	ControllerOverrideEdit<bool> primary;
	ControllerOverrideEdit<bool> secondary;
	ControllerOverrideEdit<bool> menu;
	ControllerOverrideEdit<bool> thumbClick;
};
struct ControllerWheelSweep
{
	bool enabled = false;
	XrVector3f hub{0, -0.35f, -0.45f};
	float radius = 0.18f;
	float tilt = 0.3490659f;
	float amplitude = 1.3962634f;
	float frequencyHz = 0.15f;
	float grip = 1;
	float spread = 1.5707963f;
	float receivedSeconds = 0;
};
struct ControllerMotionFrame
{
	// Preserve the separate legacy clocks: QPC samples drive glides,
	// while the wheel uses the native uptime sample with its existing phase.
	double glideSeconds = 0;
	float wheelSeconds = 0;
	ControllerHeadPose head;
	ControllerKeyboardSample keyboard;
	std::optional<ControllerPoseEdit> pose;
	std::optional<ControllerHoldEdit> hold;
	std::optional<ControllerWheelSweep> wheel;
};
struct ControllerMotionSample
{
	ControllerLocalPose local;
	XrPosef worldPose{};
	ControllerControls controls;
};
struct ControllerMotionSnapshot
{
	std::array<ControllerMotionSample, 2> hands;
	bool animationEnabled = false;
	float wheelAngle = 0;
};
struct ControllerMotionResult
{
	ControllerMotionSnapshot snapshot;
	std::optional<bool> animationToggle;
};
// Windows compatibility motion evaluator. Native polling submits one immutable
// frame value; readers see the previous or the fully updated two-hand sample.
// Keyboard/file polling and shared scheduled simulation ownership stay outside.
class LegacyControllerMotion
{
  public:
	LegacyControllerMotion();
	ControllerMotionResult Update(const ControllerMotionFrame& frame);
	ControllerMotionSnapshot Snapshot() const;
	ControllerMotionSnapshot Snapshot(ControllerHeadPose head) const;

  private:
	struct Controller
	{
		ControllerLocalPose local;
		ControllerControls controls;
	};
	struct Glide
	{
		bool active = false;
		ControllerLocalPose from;
		ControllerLocalPose to;
		double startSeconds = 0;
		double durationSeconds = 0;
	};
	struct Hold
	{
		std::optional<float> trigger;
		std::optional<float> grip;
		std::optional<float> stickX;
		std::optional<float> stickY;
		std::optional<float> yaw;
		std::optional<float> pitch;
		std::optional<float> roll;
		std::optional<bool> primary;
		std::optional<bool> secondary;
		std::optional<bool> menu;
		std::optional<bool> thumbClick;
	};
	static std::size_t HandIndex(ControllerHand hand);
	static XrPosef WorldPose(ControllerLocalPose local, ControllerHeadPose head);
	static void Trigger(Controller& controller, float value);
	static void Grip(Controller& controller, float value);
	static void ThumbClick(Controller& controller, bool value);
	static void Stick(Controller& controller, XrVector2f value);
	void AdvanceGlides(double seconds);
	std::optional<bool> ApplyKeyboard(const ControllerKeyboardSample& keyboard);
	void ApplyPose(const ControllerPoseEdit& edit);
	void ApplyHoldEdit(const ControllerHoldEdit& edit);
	void ApplyWheel(float seconds);
	void ApplyHolds();
	ControllerMotionSnapshot SnapshotLocked(ControllerHeadPose head) const;
	mutable std::mutex m_mutex;
	std::array<Controller, 2> m_controllers;
	std::array<Glide, 2> m_glides;
	std::array<Hold, 2> m_holds;
	ControllerWheelSweep m_wheel;
	ControllerHeadPose m_head;
	float m_wheelAngle = 0;
	float m_animationSeconds = 0;
	bool m_animationEnabled = false;
	bool m_animationKeyDown = false;
};
} // namespace simxr::legacy
