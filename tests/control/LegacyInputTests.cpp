#include "../TestSupport.h"
#include "math/Math.h"
#include "control/legacy/LegacyInput.h"
#include "json.h"
#include <limits>

#include <cmath>
#include <iostream>
#include <string>

namespace simxr::tests
{
bool Near(float left, float right)
{
	return std::abs(left - right) < 0.00001f;
}

void TestInputOwnership()
{
	using namespace simxr;
	LegacyInput input;
	std::string program(
		R"({"frame":0,"head":{"x":0.3,"y":1.8,"yaw":0.2,"roll":0.15},"right":{"posX":0.7,"pitch":-0.3,"trigger":0.8,"grip":4,"primary":true,"secondary":true,"stickX":-3}})"
		"\n"
		R"({"frame":1,"right":{"trigger":1e999,"primary":"invalid","stickX":-1e999}})");
	input.StartSequence(PoseSequence::Parse(program, false), {});
	input.AdvanceSequence({0});
	const auto tracking = input.Tracking(false);
	const auto expected = Compose(input.HeadPose(), XrPosef{EulerAngles(0, -0.3f, 0), {0.7f, -0.3f, -0.4f}});
	Require(Near(tracking.controllers[1].pose.pose.position.x, expected.position.x) && !tracking.focused,
			"Controller offset must compose with the owned head pose");
	const auto controls = input.Controls();
	Require(Near(controls[1].trigger, 0.8f) && controls[1].grip == 1 && controls[1].gripForce == 1 &&
				controls[1].stick.x == -1 && controls[1].trackpad.x == -1 && controls[1].primary &&
				controls[1].secondaryTouch && controls[1].triggerTouch &&
				controls[0].trigger == 0,
			"Sequence controls must clamp and derive touch aliases for only the selected hand");
	input.AdvanceSequence({1});
	Require(Near(input.Controls()[1].trigger, .8f) && input.Controls()[1].primary && input.Controls()[1].stick.x == -1,
		"Invalid sequence values replaced existing controls");
	LegacyInput anotherInstance;
	Require(anotherInstance.Controls()[1].trigger == 0 && anotherInstance.HeadAngles().roll == 0 &&
				anotherInstance.HeadPose().position.x == 0,
			"A new instance must not inherit the previous instance's controls or angles");
}

void TestSequencePlayback()
{
	using namespace simxr;
	LegacyInput input;
	std::string sequence(R"({"frame":0,"head":{"x":0.5,"y":1.8,"roll":0.2}})"
								"\n"
								R"({"frame":2,"head":{"x":2},"right":{"trigger":0.7},"capture":true})"
								"\n"
								R"({"frame":1,"head":{"x":1},"left":{"primary":true}})"
								"\n"
								R"({"frame":2,"head":{"x":3}})"
								"\n"
								R"({"frame":1e999,"head":{"x":99}})"
								"\n");
	auto prepared = PoseSequence::Parse(sequence, false);
	Require(prepared.Size() == 4, "Sequence must reject frames outside the counter range");
	input.StartSequence(std::move(prepared), {10});
	Require(!input.AdvanceSequence({9}) && input.HeadPose().position.x == 0,
			"A frame before the sequence origin must not underflow into future steps");
	Require(!input.AdvanceSequence({10}) && Near(input.HeadPose().position.x, .5f),
		"Sequence origin did not apply its initial pose");
	Require(!input.AdvanceSequence({11}) && input.HeadPose().position.x == 1 && input.Controls()[0].primary,
			"Stable frame ordering must apply the first sparse step");
	Require(input.AdvanceSequence({12}) && input.HeadPose().position.x == 3 &&
				Near(input.HeadPose().position.y, 1.8f) && Near(input.HeadAngles().roll, 0.2f) &&
				Near(input.Controls()[1].trigger, 0.7f),
			"Equal-frame order, sparse patches and capture intent must survive predecoding");
	Require(!input.AdvanceSequence({12}) && !input.AdvanceSequence({99}),
			"Consumed sequence steps must not repeat captures");
	std::string loop(R"({"frame":0,"head":{"x":4},"capture":true})"
							"\n"
							R"({"frame":2,"head":{"x":8}})"
							"\n");
	input.StartSequence(PoseSequence::Parse(loop, true), {100});
	Require(input.AdvanceSequence({100}) && !input.AdvanceSequence({102}) && input.HeadPose().position.x == 8,
			"A loop must include its last frame");
	Require(input.AdvanceSequence({106}) && input.HeadPose().position.x == 4 && !input.AdvanceSequence({106}),
			"A loop must use the current cycle after a multi-cycle jump, without repeating the same step");
	std::string empty;
	auto stopped = PoseSequence::Parse(empty, false);
	const auto count = stopped.Size();
	input.StartSequence(std::move(stopped), {});
	Require(count == 0 && !input.AdvanceSequence({107}),
			"Replacing a sequence with an empty stream must stop old playback");
}
void TestSequenceProgram()
{
	using namespace simxr;
	std::string input(R"({"frame":9007199254740993,"head":{"x":1}})"
							 "\n"
							 R"({"frame":"9007199254740993","capture":true})"
							 "\n"
							 R"({"frame":18446744073709551615,"right":{"trigger":0.5}})"
							 "\n"
							 R"({"frame":18446744073709551616,"head":{"x":99}})"
							 "\n"
							 R"({"frame":"","head":{"x":99}})"
							 "\n");
	auto program = PoseSequence::Parse(input, false);
	Require(program.Size() == 3 && program.Steps()[0].frame.value == 9007199254740993ULL &&
				program.Steps()[1].capture && program.Steps()[2].frame.value == UINT64_MAX,
			"Pose program lost exact frame identity, stable ordering or overflow validation");
	PoseSequencePlayback playback;
	playback.Start(std::move(program), {0});
	Require(playback.NextFrame() == FrameId{9007199254740993ULL} && playback.Advance({9007199254740992ULL}).empty(),
			"Pose step ran before its exact frame");
	const auto grouped = playback.Advance({9007199254740993ULL});
	Require(grouped.size() == 2 && grouped[0].head && grouped[1].capture && playback.NextFrame() == FrameId{UINT64_MAX},
			"Equal-frame steps were split or reordered");
	Require(playback.Advance({UINT64_MAX}).size() == 1 && playback.Complete() && !playback.NextFrame(),
			"Last representable frame did not finish exactly once");

	std::string loop(R"({"frame":1,"capture":true})"
							"\n"
							R"({"frame":3,"head":{"x":4}})"
							"\n");
	playback.Start(PoseSequence::Parse(loop, true), {10});
	Require(playback.NextFrame() == FrameId{11} && playback.Advance({13}).size() == 2 &&
				playback.NextFrame() == FrameId{15},
			"Loop cursor did not preserve its inclusive final frame");
	Require(playback.Advance({25}).size() == 2 && playback.Advance({25}).empty() &&
				playback.NextFrame() == FrameId{27} && !playback.Complete(),
			"Loop jump repeated a consumed step");
	bool rejected = false;
	try
	{
		playback.Start(PoseSequence({PoseSequenceStep{FrameId{2}}}, false), {UINT64_MAX});
	}
	catch (const std::overflow_error&)
	{
		rejected = true;
	}
	Require(rejected && playback.NextFrame() == FrameId{27}, "Invalid replacement changed active playback");
	std::string oversized;
	for (std::size_t index = 0; index <= PoseSequence::MaxSteps; ++index)
		oversized += "{\"frame\":0}\n";
	rejected = false;
	try
	{
		(void)PoseSequence::Parse(oversized, false);
	}
	catch (const std::length_error&)
	{
		rejected = true;
	}
	Require(rejected, "Oversized pose program silently truncated its final steps");
	playback.Start(PoseSequence{}, {});
	Require(playback.Complete() && !playback.NextFrame(), "Empty replacement did not stop playback");
}

} // namespace simxr::tests

int main()
{
	using namespace simxr::tests;
	try
	{
		for (const char* text : {R"({"value":1e300})", R"({"value":-1e300})", R"({"value":NaN})",
								 R"({"value":Infinity})", R"({"value":2147483648})", R"({"value":-2147483649})"})
			Require(json::Object(text).Number("value", 17) == 17,
					"Unrepresentable compatibility integer must preserve its default");
		Require(json::Object(R"({"value":-2.75})").Number("value", 17) == -2 &&
					json::Object(R"({"value":2147483647})").Number("value", 17) == (std::numeric_limits<int>::max)() &&
					json::Object(R"({"value":-2147483648})").Number("value", 17) == (std::numeric_limits<int>::min)(),
				"Compatibility integers must retain truncation and exact boundaries");
		Require(json::Object(R"({"value":1e40})").Number("value", 0.25f) == 0.25f &&
					json::Object(R"({"value":-1e40})").Number("value", 0.25f) == 0.25f &&
					json::Object(R"({"value":1.25e-2})").Number("value", 0.0) == 0.0125,
				"Compatibility decimals lost their value or overflowed a float");
		Require(json::Object(R"({"value":"\u03bb\ud83d\ude00"})").String("value") == "\xce\xbb\xf0\x9f\x98\x80" &&
					json::Object(R"({"value":"ok\ud800"})").String("value") == "ok" &&
					json::Object(R"({"value":"ok\udc00"})").String("value") == "ok",
				"Unicode escape decoding emitted an unpaired surrogate");
		TestInputOwnership();
		TestSequencePlayback();
		TestSequenceProgram();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
