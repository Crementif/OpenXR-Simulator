#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetProjection.h"
#include "math/Math.h"

#include <iostream>

namespace simxr::headset::tests
{
static void Mapping()
{
	const float sourceAngle = std::atan(1.f), wideAngle = std::atan(2.f), narrowAngle = std::atan(.5f);
	const XrFovf sourceFov{-sourceAngle, sourceAngle, sourceAngle, -sourceAngle};
	const XrFovf wide{-wideAngle, wideAngle, wideAngle, -wideAngle};
	const XrFovf narrow{-narrowAngle, narrowAngle, narrowAngle, -narrowAngle};
	std::array<FrameView, 2> views{{{{EulerAngles(.2f, 0, 0), {-.035f, 1.7f, 2}}, sourceFov},
									{{EulerAngles(-.15f, 0, 0), {.04f, 1.7f, 2}}, sourceFov}}};
	CompositionPlan plan;
	Require(PrepareHeadsetProjection(views, {800, 600}, {1000, 900}, {wide, narrow}, plan) == XR_SUCCESS,
			"Prepare asymmetric physical output projection");
	Require(plan.draws.size() == 2 && plan.views.size() == 2 && plan.width == 1000 && plan.height == 900,
			"Mapping lost stereo output shape");
	for (std::size_t eye = 0; eye < 2; ++eye)
	{
		Require(plan.views[eye].pose.position.x == views[eye].pose.position.x &&
					std::abs(plan.views[eye].pose.orientation.y - views[eye].pose.orientation.y) < .00001f &&
					plan.draws[eye].source.arraySlice == eye,
				"Mapping changed physical parallax/cant or eye selection");
		const auto center = EvaluateCompositionSample(plan.draws[eye], {.5f, .5f});
		Require(center && std::abs(center->x - .5f) < .00001f && std::abs(center->y - .5f) < .00001f,
				"Center ray shifted during FOV mapping");
	}
	Require(!EvaluateCompositionSample(plan.draws[0], {0, .5f}),
			"Uncovered wide-FOV ray was stretched into source pixels");
	const auto left = EvaluateCompositionSample(plan.draws[1], {0, .5f});
	Require(left && std::abs(left->x - .25f) < .00001f, "Narrower physical FOV did not select the matching source ray");
	Require(plan.views[0].fov.angleLeft == wide.angleLeft && plan.views[1].fov.angleRight == narrow.angleRight,
			"Output metadata disagrees with mapped pixels");
	Require(PrepareHeadsetProjection(std::span(views).first(1), {800, 600}, {1000, 900}, {wide, narrow}, plan) ==
				XR_ERROR_VALIDATION_FAILURE,
			"Incomplete stereo metadata accepted");
}
} // namespace simxr::headset::tests

int main()
{
	try
	{
		simxr::headset::tests::Mapping();
		std::cout << "PASS physical FOV shared-compositor ray mapping\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
