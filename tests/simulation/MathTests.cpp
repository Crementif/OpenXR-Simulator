#include "../TestSupport.h"
#include "Math.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>

namespace
{
void RequireSamePose(const XrPosef& left, const XrPosef& right)
{
	Require(std::abs(left.position.x - right.position.x) < 0.00001f &&
				std::abs(left.position.y - right.position.y) < 0.00001f &&
				std::abs(left.position.z - right.position.z) < 0.00001f,
			"Pose translation");
	Require(std::abs(left.orientation.x - right.orientation.x) < 0.00001f &&
				std::abs(left.orientation.y - right.orientation.y) < 0.00001f &&
				std::abs(left.orientation.z - right.orientation.z) < 0.00001f &&
				std::abs(left.orientation.w - right.orientation.w) < 0.00001f,
			"Pose orientation");
}

} // namespace

int main()
{
	using namespace simxr;
	XrPosef base{EulerAngles(std::numbers::pi_v<float> / 2, 0.2f, -0.1f), {3, -2, 1}};
	XrPosef child{EulerAngles(-0.4f, 0.6f, 0.3f), {0.5f, 1, -0.8f}};
	RequireSamePose(Compose(IdentityPose(), child), child);
	RequireSamePose(Compose(base, IdentityPose()), base);
	RequireSamePose(RelativePose(Compose(base, child), base), child);
	RequireSamePose(RelativePose(base, base), IdentityPose());
	auto rotated = Rotate(EulerAngles(std::numbers::pi_v<float> / 2, 0, 0), {0, 0, -1});
	Require(std::abs(rotated.x + 1) < 0.00001f && std::abs(rotated.z) < 0.00001f, "Yaw convention");
	const XrPosef drifted{{0, 0, 0, 1.005f}, {1, 2, 3}};
	const auto normalized = NormalizedPose(drifted);
	Require(normalized.has_value(), "A finite pose within quaternion tolerance was rejected");
	RequireSamePose(*normalized, {{0, 0, 0, 1}, {1, 2, 3}});
	Require(drifted.orientation.w == 1.005f, "Pose normalization mutated its source value");
	for (const float component : {0.0f, 1.02f, std::numeric_limits<float>::infinity(),
								  std::numeric_limits<float>::quiet_NaN(), std::numeric_limits<float>::max()})
	{
		auto invalid = IdentityPose();
		invalid.orientation.w = component;
		Require(!NormalizedPose(invalid), "Invalid or nonfinite rotation was normalized");
	}
	for (const float component : {std::numeric_limits<float>::infinity(), std::numeric_limits<float>::quiet_NaN()})
	{
		auto invalid = IdentityPose();
		for (auto* position : {&invalid.position.x, &invalid.position.y, &invalid.position.z})
		{
			*position = component;
			Require(!NormalizedPose(invalid), "Nonfinite pose position was accepted");
			*position = 0;
		}
	}
	std::cout << "Math tests passed\n";
}
