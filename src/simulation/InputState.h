#pragma once

#include "Types.h"

#include <array>
#include <cstdint>
#include <mutex>
#include <openxr/openxr.h>
#include <optional>
#include <string>
#include <vector>

namespace simxr
{
struct ControllerControls
{
	float trigger = 0;
	float grip = 0;
	float gripForce = 0;
	XrVector2f stick{};
	XrVector2f trackpad{};
	float trackpadForce = 0;
	bool primary = false;
	bool secondary = false;
	bool menu = false;
	bool back = false;
	bool thumbClick = false;
	bool trackpadClick = false;
	bool primaryTouch = false;
	bool secondaryTouch = false;
	bool triggerTouch = false;
	bool thumbTouch = false;
	bool trackpadTouch = false;
	bool thumbrestTouch = false;
	bool connected = true;
	std::optional<bool> triggerClick;
	std::string physicalProfile;
};

enum class InputDeviceKind
{
	Controller,
	EyeGaze,
	Tracker
};

struct InputDeviceId
{
	InputDeviceKind kind = InputDeviceKind::Controller;
	std::uint64_t value = 0;
	bool operator==(const InputDeviceId&) const = default;
};

struct InputDevice
{
	InputDeviceId identity;
	std::vector<std::string> userPaths;
	std::string preferredProfile;
	bool connected = true;
	bool poseAvailable = true;
	ControllerControls controls;
};

struct InputSnapshot
{
	std::uint64_t revision = 0;
	RuntimeTime time;
	std::array<ControllerControls, 2> controllers;
	std::vector<InputDevice> devices;
	std::array<std::string, 2> preferredProfiles{"/interaction_profiles/oculus/touch_controller",
												 "/interaction_profiles/oculus/touch_controller"};
};

class InputState
{
  public:
	static XrResult Validate(const InputSnapshot& snapshot);
	XrResult Publish(InputSnapshot snapshot);
	InputSnapshot Snapshot() const;
	std::uint64_t Revision() const;
	void Reset();
	// Allocation-free release of synthetic held buttons/axes; identity, profile,
	// connection and sample time remain observable.
	void ReleaseControls(std::array<bool, 2> hands = {true, true});

  private:
	mutable std::mutex m_mutex;
	InputSnapshot m_snapshot;
};
} // namespace simxr
