#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetSpace.h"
#include "math/Math.h"

#include <iostream>

namespace simxr::tests
{
static void Changes()
{
	headset::HeadsetSpace space;
	const XrPosef first{EulerAngles(.5f, 0, 0), {2, 0, -1}};
	const XrPosef second{EulerAngles(-.2f, 0, 0), {1, .1f, 3}};
	space.Schedule(100, true, first);
	space.Schedule(200, true, second);
	const auto before = space.At(99);
	Require(before.revision == 1 && before.initialFromCurrent.position.x == 0,
			"Polling does not apply a future origin change");
	const auto at = space.At(100);
	Require(at.revision == 2 && at.initialFromCurrent.position.x == 2, "Change applies at its exact effective time");
	const auto after = space.At(200);
	const auto expected = Compose(first, second);
	Require(after.revision == 3 && std::abs(after.initialFromCurrent.position.z - expected.position.z) < .00001f &&
				std::abs(after.initialFromCurrent.orientation.y - expected.orientation.y) < .00001f,
			"Compose new natural origins in order, including rotations");
	Require(before.revision == 1 && at.revision == 2, "Previously sampled calibration stays immutable");
	space.Schedule(300, false, {});
	Require(space.At(299).revision == 3, "Invalid future origin does not invalidate earlier samples");
	bool failed = false;
	try
	{
		space.At(300);
	}
	catch (const std::runtime_error&)
	{
		failed = true;
	}
	Require(failed, "Unknown origin suspends mapping at its effective time");
	failed = false;
	try
	{
		space.At(301);
	}
	catch (const std::runtime_error&)
	{
		failed = true;
	}
	Require(failed, "Mapping cannot silently recover without realignment");
	headset::HeadsetSpace late;
	late.At(100);
	failed = false;
	try
	{
		late.Schedule(99, true, first);
	}
	catch (const std::runtime_error&)
	{
		failed = true;
	}
	Require(failed, "Late events cannot rewrite already sampled tracking");
}
} // namespace simxr::tests
int main()
{
	try
	{
		simxr::tests::Changes();
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
