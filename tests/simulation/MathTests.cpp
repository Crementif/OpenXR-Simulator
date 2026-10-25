#include "../TestSupport.h"
#include "math/Math.h"

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

namespace
{
void WideTransforms()
{
	using namespace simxr;
	const auto rotation = EulerAngles(0, 0, std::numbers::pi_v<float> / 4);
	const auto relative = RelativePose({{0, 0, 0, 1}, {2e38f, 0, 0}}, {rotation, {-2e38f, 0, 0}});
	Require(std::isfinite(relative.position.x) && std::isfinite(relative.position.y) &&
				std::abs(relative.position.x / 2.8284271e38f - 1) < 0.00001f &&
				std::abs(relative.position.y / -2.8284271e38f - 1) < 0.00001f,
			"Representable relative transform overflowed before rotation");
	const auto composed = Compose({rotation, {0, -2e38f, 0}}, {{0, 0, 0, 1}, {3e38f, 3e38f, 0}});
	Require(std::isfinite(composed.position.x) && std::isfinite(composed.position.y) &&
				std::abs(composed.position.y / 2.2426407e38f - 1) < 0.00001f,
			"Representable composition overflowed before translation");
}
} // namespace

int main()
{
	WideTransforms();
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
