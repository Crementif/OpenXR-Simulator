#include "simulation/HapticState.h"
#include "../TestSupport.h"

#include <array>
#include <algorithm>
#include <iostream>
#include <limits>

int main()
{
	using namespace simxr;
	HapticState state(2);
	std::array left{HapticDevice::LeftController};
	std::array both{HapticDevice::LeftController, HapticDevice::RightController};
	auto action = reinterpret_cast<XrAction>(1);
	auto otherAction = reinterpret_cast<XrAction>(2);
	XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION, nullptr, 50, 100, 0.5f};
	Require(state.Apply(left, action, 3, vibration, {Duration{100}}, false) == XR_SESSION_NOT_FOCUSED, "Unfocused application");
	Require(state.Snapshot({Duration{100}}).empty(), "Unfocused pulse created output");
	Require(state.Apply(left, action, 3, vibration, {Duration{100}}, true) == XR_SUCCESS, "Apply pulse");
	auto saved = state.Snapshot({Duration{120}});
	Require(saved.size() == 1 && saved[0].end->value.count() == 150 && saved[0].status == HapticStatus::Active, "Effective duration");
	Require(state.Snapshot({Duration{150}})[0].status == HapticStatus::Expired, "Exact expiration");
	Require(saved[0].status == HapticStatus::Active, "Retained snapshot changed");
	vibration.duration = XR_INFINITE_DURATION;
	Require(state.Apply(both, action, 0, vibration, {Duration{160}}, true) == XR_SUCCESS, "Infinite pulse");
	Require(!state.Snapshot({Duration{200}}).back().end, "Infinite duration overflowed");
	Require(state.Apply(left, otherAction, 3, vibration, {Duration{200}}, true) == XR_SUCCESS, "Replacement pulse");
	auto outputs = state.Snapshot({Duration{210}});
	Require(outputs.size() == 4 && outputs[1].status == HapticStatus::Replaced, "Replacement on same device");
	Require(state.Stop(left, action, {Duration{220}}, true) == XR_SUCCESS, "Stop old action");
	Require(state.Snapshot({Duration{220}}).back().status == HapticStatus::Active, "Old action stopped replacement");
	state.LoseFocus({Duration{230}});
	outputs = state.Snapshot({Duration{230}});
	Require(outputs.size() == 2 && outputs[0].status == HapticStatus::FocusLost && outputs[1].status == HapticStatus::FocusLost, "Focus loss/bounded history");
	state.Reset();
	vibration.duration = XR_MIN_HAPTIC_DURATION;
	Require(state.Apply(left, action, 3, vibration, {Duration{10}}, true) == XR_SUCCESS, "Minimal duration");
	Require(state.Snapshot({Duration{11}})[0].end->value.count() == 1000010, "Minimal effective duration");
	vibration.amplitude = std::numeric_limits<float>::quiet_NaN();
	Require(state.Apply(left, action, 3, vibration, {Duration{10}}, true) == XR_ERROR_VALIDATION_FAILURE, "NaN pulse");
	HapticState typed(16);
	const InputDeviceId trackerOne{InputDeviceKind::Tracker, 1}, trackerTwo{InputDeviceKind::Tracker, 2};
	const InputDeviceId rightController{InputDeviceKind::Controller, 1};
	const std::array typedTargets{trackerOne, rightController, trackerOne, trackerTwo};
	vibration.amplitude = 0.5f;
	vibration.duration = XR_INFINITE_DURATION;
	Require(typed.Apply(typedTargets, action, 123, vibration, {Duration{100}}, true) == XR_SUCCESS, "Typed haptic apply");
	auto typedSaved = typed.Snapshot({Duration{100}});
	Require(typedSaved.size() == 3 && std::count_if(typedSaved.begin(), typedSaved.end(), [&](const auto& output) { return output.device == trackerOne; }) == 1,
		"Tracker aliases scheduled duplicate haptic output");
	const std::array duplicateTracker{trackerOne, trackerOne};
	vibration.amplitude = 0.8f;
	Require(typed.Apply(duplicateTracker, otherAction, 456, vibration, {Duration{200}}, true) == XR_SUCCESS, "Tracker replacement");
	auto typedOutputs = typed.Snapshot({Duration{200}});
	Require(typedOutputs.size() == 4 && typedOutputs[0].status == HapticStatus::Replaced && typedOutputs.back().device == trackerOne &&
		typedOutputs.back().action == otherAction && typedOutputs.back().subactionPath == 456, "Typed replacement metadata");
	Require(typed.Stop(duplicateTracker, action, {Duration{210}}, true) == XR_SUCCESS && typed.Snapshot({Duration{210}}).back().status == HapticStatus::Active,
		"Old tracker action stopped replacement");
	Require(typed.Stop(duplicateTracker, otherAction, {Duration{220}}, true) == XR_SUCCESS && typed.Snapshot({Duration{220}}).back().status == HapticStatus::Stopped,
		"Typed tracker stop");
	const std::array legacyRight{HapticDevice::RightController};
	Require(typed.Apply(legacyRight, otherAction, 0, vibration, {Duration{225}}, true) == XR_SUCCESS, "Legacy controller identity adapter");
	typedOutputs = typed.Snapshot({Duration{225}});
	Require(std::count_if(typedOutputs.begin(), typedOutputs.end(), [&](const auto& output)
		{ return output.device == rightController && output.status == HapticStatus::Active; }) == 1 &&
		std::count_if(typedOutputs.begin(), typedOutputs.end(), [&](const auto& output)
		{ return output.device == trackerTwo && output.status == HapticStatus::Active; }) == 1, "Device kind collision or cross-device replacement");
	typed.RemoveAction(action, {Duration{230}});
	typedOutputs = typed.Snapshot({Duration{230}});
	Require(std::none_of(typedOutputs.begin(), typedOutputs.end(), [&](const auto& output) { return output.action == action && output.status == HapticStatus::Active; }),
		"Action teardown retained tracker haptics");
	const auto beforeInvalid = typedOutputs.size();
	const std::array invalidTargets{trackerOne, InputDeviceId{InputDeviceKind::EyeGaze, 0}};
	Require(typed.Apply(invalidTargets, action, 0, vibration, {Duration{240}}, true) == XR_ERROR_VALIDATION_FAILURE &&
		typed.Snapshot({Duration{240}}).size() == beforeInvalid, "Invalid typed target partially changed output");
	const std::array zeroTracker{InputDeviceId{InputDeviceKind::Tracker, 0}};
	Require(typed.Stop(zeroTracker, action, {Duration{240}}, true) == XR_ERROR_VALIDATION_FAILURE, "Invalid tracker identity accepted");
	typed.LoseFocus({Duration{250}});
	typedOutputs = typed.Snapshot({Duration{250}});
	Require(typedOutputs.back().status == HapticStatus::FocusLost && typedSaved[0].status == HapticStatus::Active,
		"Typed focus loss/retained output snapshot");
	std::cout << "Haptic state tests passed\n";
}
