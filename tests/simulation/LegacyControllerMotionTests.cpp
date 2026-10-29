#include "../TestSupport.h"
#include "math/Math.h"
#include "simulation/LegacyControllerMotion.h"

#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <thread>

namespace simxr::legacy::tests
{
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
	return frame;
}

void FiniteGlideEndpoints()
{
	LegacyControllerMotion motion;
	const auto largest = std::numeric_limits<float>::max();
	auto frame = Frame(0);
	frame.pose = ControllerPoseEdit{};
	frame.pose->position = {largest, 0, 0};
	motion.Update(frame);
	frame.pose->position.x = -largest;
	frame.pose->duration = std::chrono::milliseconds{1000};
	motion.Update(frame);
	const auto midpoint = motion.Update(Frame(.5)).snapshot;
	Require(std::isfinite(midpoint.hands[1].local.position.x) && midpoint.hands[1].local.position.x == 0,
			"Finite glide endpoints overflowed at their midpoint");
	Require(motion.Update(Frame(1)).snapshot.hands[1].local.position.x == -largest,
			"Finite glide endpoint was not preserved");
}

void FiniteMotionIntermediates()
{
	LegacyControllerMotion motion;
	const auto largest = std::numeric_limits<float>::max();
	auto frame = Frame(0);
	frame.pose = ControllerPoseEdit{};
	frame.pose->position = {largest, 0, -largest};
	frame.head = {{largest, 0, largest}, std::numbers::pi_v<float> / 4, 0};
	const auto position = motion.Update(frame).snapshot.hands[1].worldPose.position;
	const bool finitePosition = std::isfinite(position.x) && std::isfinite(position.z) &&
								std::abs(position.x / largest - 1) < .00001f &&
								std::abs(position.z / largest - (1 - std::sqrt(2.0f))) < .00001f;
	frame.pose->position = {};
	frame.pose->yaw = largest;
	frame.head = {{}, largest, 0};
	const auto orientation = motion.Update(frame).snapshot.hands[1].worldPose.orientation;
	const bool finiteOrientation = std::isfinite(orientation.x) && std::isfinite(orientation.y) &&
								   std::isfinite(orientation.z) && std::isfinite(orientation.w);
	// Equal head/local yaw must preserve the runtime library's rotation without
	// overflowing their float sum. Constant folding uses a different reducer on MinGW.
	volatile double runtimeYaw = frame.head.yaw;
	const auto yawSine = static_cast<float>(std::sin(runtimeYaw));
	const auto yawCosine = static_cast<float>(std::cos(runtimeYaw));
	const auto expectedOrientation =
		simxr::Multiply({0, yawSine, 0, yawCosine}, simxr::EulerAngles(0, frame.pose->pitch, 0));
	Near(orientation.x, expectedOrientation.x, "Wide yaw changed orientation");
	Near(orientation.y, expectedOrientation.y, "Wide yaw changed orientation");
	Near(orientation.z, expectedOrientation.z, "Wide yaw changed orientation");
	Near(orientation.w, expectedOrientation.w, "Wide yaw changed orientation");
	Require(finitePosition && finiteOrientation, "Finite motion input overflowed a representable result");
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

void KeyboardFileAndHoldPrecedence()
{
	LegacyControllerMotion motion;
	auto frame = Frame(1.25);
	frame.keyboard.active = true;
	frame.keyboard.frameSeconds = 1;
	frame.keyboard.translationDirection = {1, 1, 1};
	frame.keyboard.hands[1].trigger = true;
	frame.keyboard.hands[1].primary = true;
	frame.keyboard.hands[0].grip = frame.keyboard.hands[1].grip = true;
	frame.pose = ControllerPoseEdit{};
	frame.pose->position = {9, 9, 9};
	frame.pose->trigger = 0.2f;
	frame.pose->primary = false;
	frame.hold = ControllerHoldEdit{};
	frame.hold->hand = ControllerHand::Right;
	frame.hold->grip = ControllerOverrideEdit<float>::Set(0);
	frame.hold->primary = ControllerOverrideEdit<bool>::Set(true);
	frame.hold->stickX = ControllerOverrideEdit<float>::Set(-0.75f);
	frame.hold->yaw = ControllerOverrideEdit<float>::Set(0.4f);
	frame.hold->roll = ControllerOverrideEdit<float>::Set(-0.7f);
	auto result = motion.Update(frame);
	const auto right = result.snapshot.hands[1];
	Position(right.local.position, {9, 9, 9}, "File pose did not override keyboard movement");
	Near(right.local.yaw, 0.4f, "Sticky yaw did not override file orientation");
	Near(right.local.roll, -0.7f, "Sticky roll was not applied");
	Near(right.controls.grip, 0, "Forced-off hold did not override keyboard grip");
	Near(result.snapshot.hands[0].controls.grip, 1, "Right-hand hold affected the other hand");
	Near(right.controls.trigger, 0.2f, "One-shot trigger did not override keyboard");
	Require(right.controls.primary && !right.controls.triggerTouch, "Control aliases differ from thresholded controls");
	Near(right.controls.trackpad.x, -0.75f, "Held stick no longer aliases the legacy trackpad");

	frame = Frame(1.5);
	frame.keyboard.active = true;
	frame.keyboard.hands[1].grip = true;
	frame.hold = ControllerHoldEdit{};
	frame.hold->hand = ControllerHand::Right;
	frame.hold->grip = ControllerOverrideEdit<float>::Release();
	result = motion.Update(frame);
	Near(result.snapshot.hands[1].controls.grip, 1, "Released grip override did not return to keyboard input");
	Near(result.snapshot.hands[1].controls.trigger, 0, "One-shot trigger became a sticky override");
	Near(result.snapshot.hands[1].local.yaw, 0.4f, "Partial hold update cleared a different channel");
	frame = Frame(1.75);
	frame.keyboard.active = true;
	frame.pose = ControllerPoseEdit{};
	frame.hold = ControllerHoldEdit{};
	frame.hold->clear = true;
	result = motion.Update(frame);
	Near(result.snapshot.hands[1].local.yaw, 0, "Clear did not return yaw ownership to the pose input");
	Near(result.snapshot.hands[1].local.roll, -0.7f, "Clear unexpectedly reset the last roll");
	Require(!result.snapshot.hands[1].controls.primary, "Clear did not return buttons to keyboard state");

	frame = Frame(2);
	frame.keyboard.active = true;
	frame.keyboard.translationDirection.x = 1;
	frame.keyboard.frameSeconds = 1;
	const auto before = motion.Snapshot().hands[1].local.position;
	result = motion.Update(frame);
	Near(result.snapshot.hands[1].local.position.x, before.x + 0.5f, "Cleared holds blocked keyboard movement");
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
	frame.keyboard.active = true;
	std::atomic<bool> finished = false, inconsistent = false;
	std::thread reader([&] {
		do
		{
			const auto sample = motion.Snapshot();
			if (sample.hands[0].controls.primary != sample.hands[1].controls.primary)
				inconsistent = true;
		}
		while (!finished.load());
	});
	for (unsigned index = 0; index < 1000; ++index)
	{
		frame.keyboard.hands[0].primary = frame.keyboard.hands[1].primary = index % 2 != 0;
		motion.Update(frame);
	}
	finished = true;
	reader.join();
	Require(!inconsistent, "An input reader observed only one side of a controller update");
}
} // namespace simxr::legacy::tests

int main()
{
	using namespace simxr::legacy::tests;
	try
	{
		FiniteMotionIntermediates();
		FiniteGlideEndpoints();
		HeadSpaceAndTimedMoves();
		KeyboardFileAndHoldPrecedence();
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
