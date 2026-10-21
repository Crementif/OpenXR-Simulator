#include "../TestSupport.h"
#include "Math.h"
#include "simulation/LegacyControllerMotion.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <numbers>
#include <thread>

namespace
{
using namespace simxr::legacy;
void Near(float actual, float expected, const char* message, float tolerance = 0.0001f)
{
	Require(std::abs(actual - expected) <= tolerance, message);
}
void Position(XrVector3f actual, XrVector3f expected, const char* message)
{
	Near(actual.x, expected.x, message);
	Near(actual.y, expected.y, message);
	Near(actual.z, expected.z, message);
}
ControllerMotionFrame Frame(double seconds)
{
	ControllerMotionFrame frame;
	frame.glideSeconds = seconds;
	frame.wheelSeconds = static_cast<float>(seconds);
	return frame;
}
void HeadSpaceAndTimedMoves()
{
	LegacyControllerMotion motion;
	const auto original = motion.Snapshot();
	const auto pointing = simxr::Rotate(original.hands[1].worldPose.orientation, {0, 0, -1});
	Near(pointing.y, 1, "Default controller must point upward");
	Position(original.hands[0].local.position, {-0.2f, -0.3f, -0.4f}, "Left default pose changed");
	Position(original.hands[1].local.position, {0.2f, -0.3f, -0.4f}, "Right default pose changed");
	ControllerHeadPose head{{2, 3, 4}, std::numbers::pi_v<float> / 2, 0.7f};
	const auto rotated = motion.Snapshot(head);
	Position(rotated.hands[1].worldPose.position, {1.6f, 2.7f, 3.8f}, "Controller offset must rotate by head yaw only");
	const auto expected = simxr::EulerAngles(head.yaw, head.pitch + 1.5707963268f, 0);
	Near(rotated.hands[1].worldPose.orientation.x, expected.x, "Controller pitch did not follow head pitch");
	Near(rotated.hands[1].worldPose.orientation.z, expected.z, "Controller orientation order changed");

	auto frame = Frame(1);
	frame.pose = ControllerPoseEdit{};
	frame.pose->position = {1.2f, -0.1f, -0.6f};
	frame.pose->yaw = 1;
	frame.pose->pitch = 0.2f;
	frame.pose->duration = std::chrono::milliseconds(1000);
	frame.pose->receivedSeconds = 1;
	frame.pose->trigger = 0.75f;
	frame.pose->primary = true;
	auto result = motion.Update(frame);
	Position(result.snapshot.hands[1].local.position, original.hands[1].local.position, "Glide jumped at admission");
	Require(result.snapshot.hands[1].controls.primary && result.snapshot.hands[1].controls.triggerTouch,
			"One-shot controls must apply when a timed move is admitted");
	result = motion.Update(Frame(1.5));
	Position(result.snapshot.hands[1].local.position, {0.7f, -0.2f, -0.5f}, "Glide midpoint is not linear");
	Near(result.snapshot.hands[1].local.yaw, 0.5f, "Glide yaw midpoint changed");
	Near(result.snapshot.hands[1].local.pitch, (1.5707963268f + 0.2f) / 2, "Glide pitch midpoint changed");
	result = motion.Update(Frame(2.5));
	Position(result.snapshot.hands[1].local.position, frame.pose->position, "Glide did not clamp at its endpoint");
	Position(original.hands[1].local.position, {0.2f, -0.3f, -0.4f}, "A copied observation changed after update");

	frame = Frame(3);
	frame.pose = ControllerPoseEdit{};
	frame.pose->position = {3.2f, -0.1f, -0.6f};
	frame.pose->duration = std::chrono::milliseconds(1000);
	frame.pose->receivedSeconds = 3;
	motion.Update(frame);
	frame = Frame(3.5);
	frame.pose = ControllerPoseEdit{};
	frame.pose->position = {-0.8f, -0.1f, -0.6f};
	frame.pose->duration = std::chrono::milliseconds(500);
	frame.pose->receivedSeconds = 3.5;
	motion.Update(frame);
	Near(motion.Update(Frame(3.75)).snapshot.hands[1].local.position.x, 0.7f,
		 "Replacement glide did not start from the current interpolated pose");
	frame = Frame(3.8);
	frame.pose = ControllerPoseEdit{};
	frame.pose->position = {9, 8, 7};
	motion.Update(frame);
	Position(motion.Update(Frame(6)).snapshot.hands[1].local.position, {9, 8, 7}, "Jump did not cancel an older glide");
}
void KeyboardFileWheelAndHoldPrecedence()
{
	LegacyControllerMotion motion;
	auto frame = Frame(1.25);
	frame.keyboard.active = true;
	frame.keyboard.frameSeconds = 1;
	frame.keyboard.translationDirection = {1, 1, 1};
	frame.keyboard.hands[1].trigger = true;
	frame.keyboard.hands[1].primary = true;
	frame.pose = ControllerPoseEdit{};
	frame.pose->position = {9, 9, 9};
	frame.pose->trigger = 0.2f;
	frame.pose->primary = false;
	frame.wheel = ControllerWheelSweep{};
	frame.wheel->enabled = true;
	frame.wheel->hub = {0, 0, 0};
	frame.wheel->radius = 2;
	frame.wheel->tilt = 0;
	frame.wheel->amplitude = 0.3f;
	frame.wheel->frequencyHz = 1;
	frame.wheel->receivedSeconds = 1;
	frame.hold = ControllerHoldEdit{};
	frame.hold->hand = ControllerHand::Right;
	frame.hold->grip = ControllerOverrideEdit<float>::Set(0);
	frame.hold->primary = ControllerOverrideEdit<bool>::Set(true);
	frame.hold->stickX = ControllerOverrideEdit<float>::Set(-0.75f);
	frame.hold->yaw = ControllerOverrideEdit<float>::Set(0.4f);
	frame.hold->roll = ControllerOverrideEdit<float>::Set(-0.7f);
	auto result = motion.Update(frame);
	const auto right = result.snapshot.hands[1];
	Near(result.snapshot.wheelAngle, 0.3f, "Wheel clock phase changed");
	Position(right.local.position, {2 * std::cos(0.3f), -2 * std::sin(0.3f), 0}, "Wheel did not override file pose");
	Near(right.local.yaw, 0.4f, "Sticky yaw did not override wheel orientation");
	Near(right.local.roll, -0.7f, "Sticky roll did not override wheel roll");
	Near(right.controls.grip, 0, "Forced-off hold did not override wheel grip");
	Near(result.snapshot.hands[0].controls.grip, 1, "Right-hand hold affected the other hand");
	Near(right.controls.trigger, 0.2f, "One-shot trigger did not override keyboard");
	Require(right.controls.primary && !right.controls.triggerTouch, "Control aliases differ from thresholded controls");
	Near(right.controls.trackpad.x, -0.75f, "Held stick no longer aliases the legacy trackpad");

	frame = Frame(1.5);
	frame.keyboard.active = true;
	frame.hold = ControllerHoldEdit{};
	frame.hold->hand = ControllerHand::Right;
	frame.hold->grip = ControllerOverrideEdit<float>::Release();
	result = motion.Update(frame);
	Near(result.snapshot.hands[1].controls.grip, 1, "Released grip override did not return to wheel input");
	Near(result.snapshot.hands[1].controls.trigger, 0, "One-shot trigger became a sticky override");
	Near(result.snapshot.hands[1].local.yaw, 0.4f, "Partial hold update cleared a different channel");
	frame = Frame(1.75);
	frame.keyboard.active = true;
	frame.hold = ControllerHoldEdit{};
	frame.hold->clear = true;
	result = motion.Update(frame);
	Near(result.snapshot.hands[1].local.yaw, 0, "Clear did not return yaw ownership to the wheel");
	Near(result.snapshot.hands[1].local.roll, -0.3f, "Clear did not return roll ownership to the wheel");
	Require(!result.snapshot.hands[1].controls.primary, "Clear did not return buttons to keyboard state");

	frame = Frame(2);
	frame.keyboard.active = true;
	frame.wheel = ControllerWheelSweep{}; // disable without resetting the last pose
	frame.keyboard.translationDirection.x = 1;
	frame.keyboard.frameSeconds = 1;
	const auto before = motion.Snapshot().hands[1].local.position;
	result = motion.Update(frame);
	Near(result.snapshot.hands[1].local.position.x, before.x + 0.5f, "Disabled wheel still overrode keyboard movement");
	frame = Frame(2.5);
	frame.hold = ControllerHoldEdit{};
	frame.hold->thumbClick = ControllerOverrideEdit<bool>::Set(true);
	motion.Update(frame);
	frame = Frame(3);
	frame.keyboard.active = true;
	frame.keyboard.hands[1].thumbClick = false;
	frame.hold = ControllerHoldEdit{};
	frame.hold->clear = true;
	result = motion.Update(frame);
	Require(result.snapshot.hands[0].controls.thumbClick && !result.snapshot.hands[1].controls.thumbClick,
			"A missing keyboard channel invented a release unlike the legacy path");
}
void AnimationEdgesAndReset()
{
	LegacyControllerMotion motion;
	auto frame = Frame(1);
	frame.keyboard.active = true;
	frame.keyboard.animationKeyDown = true;
	frame.keyboard.frameSeconds = 1;
	frame.keyboard.translationDirection = {100, 100, 100};
	auto result = motion.Update(frame);
	Require(result.animationToggle == true && result.snapshot.animationEnabled,
			"Animation did not toggle on at a key edge");
	Near(result.snapshot.hands[1].local.position.x, 0.4f * std::sin(1.0f),
		 "Animation did not take precedence over numpad movement");
	result = motion.Update(frame);
	Require(!result.animationToggle, "A held M key toggled repeatedly");
	frame.keyboard.active = false;
	frame.keyboard.animationKeyDown = false;
	motion.Update(frame);
	frame.keyboard.active = true;
	frame.keyboard.animationKeyDown = true;
	Require(!motion.Update(frame).animationToggle, "Inactive polling changed the key edge history");
	frame.keyboard.animationKeyDown = false;
	motion.Update(frame);
	frame.keyboard.animationKeyDown = true;
	frame.keyboard.resetRight = true;
	result = motion.Update(frame);
	Require(result.animationToggle == false && !result.snapshot.animationEnabled,
			"Animation key edge or reset precedence changed");
	Position(result.snapshot.hands[1].local.position, {0.2f, -0.3f, -0.4f},
			 "Numpad reset did not restore the right pose");
}
void AtomicPublicationAndInvalidHand()
{
	LegacyControllerMotion motion;
	auto frame = Frame(1);
	frame.keyboard.active = true;
	frame.keyboard.hands[1].trigger = true;
	frame.pose = ControllerPoseEdit{};
	frame.pose->hand = static_cast<ControllerHand>(7);
	bool rejected = false;
	try
	{
		motion.Update(frame);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && motion.Snapshot().hands[1].controls.trigger == 0,
			"An invalid hand partially published keyboard state");
	frame = Frame(1);
	frame.wheel = ControllerWheelSweep{};
	frame.wheel->enabled = true;
	frame.wheel->amplitude = 0;
	frame.wheel->tilt = 0;
	motion.Update(frame);
	std::atomic<bool> finished = false, inconsistent = false;
	std::thread reader(
		[&]
		{
			do
			{
				const auto sample = motion.Snapshot();
				if (std::abs(sample.hands[1].local.position.x - sample.hands[0].local.position.x - 0.36f) > 0.001f ||
					sample.hands[0].controls.primary != sample.hands[1].controls.primary)
					inconsistent = true;
			} while (!finished.load());
		});
	for (unsigned index = 0; index < 1000; ++index)
	{
		frame.wheel->hub.x = static_cast<float>(index);
		frame.hold = ControllerHoldEdit{};
		frame.hold->primary = ControllerOverrideEdit<bool>::Set(index % 2 != 0);
		motion.Update(frame);
	}
	finished = true;
	reader.join();
	Require(!inconsistent, "An input reader observed only one side of a controller update");
}
} // namespace
int main()
{
	try
	{
		HeadSpaceAndTimedMoves();
		KeyboardFileWheelAndHoldPrecedence();
		AnimationEdgesAndReset();
		AtomicPublicationAndInvalidHand();
		std::cout << "Legacy controller motion tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
