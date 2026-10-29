#include "../TestSupport.h"
#include "math/Math.h"
#include "control/legacy/LegacyInput.h"
#include "simulation/PoseSequence.h"
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

void TestStandaloneInput()
{
	using namespace simxr;
	LegacyInput input;
	const auto tracking = input.Tracking(false);
	Require(!tracking.focused && Near(tracking.head.pose.position.y, 1.7f) &&
		Near(tracking.controllers[0].pose.pose.position.x, -.2f) &&
		Near(tracking.controllers[1].pose.pose.position.x, .2f) && input.Controls()[0].trigger == 0,
		"Standalone input retains neutral head-relative controller defaults");
	auto views = input.Views();
	views.outerWidth = 512;
	input.SetViews(views);
	Require(input.Views().outerWidth == 512 && LegacyInput{}.Views().outerWidth == 1280,
		"Standalone view settings remain instance-owned");
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
		TestStandaloneInput();
		TestSequenceProgram();
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
