#include "simulation/ViewConfiguration.h"
#include "simulation/TrackingSystem.h"
#include "math/Math.h"
#include "../TestSupport.h"

#include <cmath>
#include <iostream>
#include <limits>

namespace
{
void InsetBoundaries()
{
	using namespace simxr;
	ViewSettings settings;
	settings.outerFov = {{{-0.9f, 0.8f, 0.8f, -0.9f}, {-0.9f, 0.8f, 0.8f, -0.9f}}};
	settings.insetHalfAngle = 0.85f;
	bool rejected = false;
	try
	{
		(void)ViewConfiguration(ViewLayout::StereoWithInset, settings);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Rounded inset width admitted reversed clamp bounds");
	for (auto halfAngle : {std::nextafter(0.85f, 0.0f), std::numeric_limits<float>::denorm_min()})
	{
		settings.insetHalfAngle = halfAngle;
		ViewConfiguration views(ViewLayout::StereoWithInset, settings);
		for (auto gaze : {-1.4f, 0.0f, 0.5f, 1.4f})
		{
			TrackingState input;
			input.gaze.yaw = gaze;
			input.gaze.pitch = gaze;
			const auto snapshot = TrackingSystem::Evaluate({Duration{1}}, input, BodyModel{});
			const auto located = views.Locate(snapshot, true);
			for (std::size_t index = 2; index < 4; ++index)
			{
				const auto& inset = located.views[index].fov;
				const auto& outer = settings.outerFov[index - 2];
				Require(inset.angleLeft >= outer.angleLeft && inset.angleRight <= outer.angleRight &&
							inset.angleDown >= outer.angleDown && inset.angleUp <= outer.angleUp &&
							inset.angleLeft < inset.angleRight && inset.angleDown < inset.angleUp,
						"Inset rounding escaped the outer FOV or collapsed its extent");
			}
		}
	}
}
} // namespace

int main()
{
	Require(simxr::ParseLaunchRenderExtent("2048") == 2048 &&
		simxr::ParseLaunchRenderExtent("2300") == 2300 &&
		simxr::ParseLaunchRenderExtent("-1") == 16 &&
		simxr::ParseLaunchRenderExtent("99999") == 8192,
		"Launch extent parsing changed its shared bounds");
	for (const auto* invalid : {"", "2048x", " 2048", "+2048", "18446744073709551616"})
	{
		bool rejected = false;
		try
		{
			(void)simxr::ParseLaunchRenderExtent(invalid);
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Malformed launch extent accepted");
	}
	InsetBoundaries();
	using namespace simxr;
	TrackingSystem tracking;
	TrackingState state;
	state.head.pose.orientation = EulerAngles(1.5707963268f, 0, 0);
	auto snapshot = tracking.Publish({Duration{100}}, state);
	ViewConfiguration stereo;
	Require(stereo.EyeCount() == 2 && stereo.Describe().size() == 2, "Stereo views");
	auto views = stereo.Locate(snapshot);
	Require(views.head.sampleTime == snapshot.head.sampleTime && views.head.flags == snapshot.head.flags &&
				views.head.pose.orientation.y == snapshot.head.pose.orientation.y,
			"Located eyes lost the head from their exact tracking sample");
	Require(std::abs(views.views[0].pose.pose.position.z - 0.032f) < 0.00001f, "Rotated eye separation");
	Require(std::abs(views.views[1].pose.pose.position.z + 0.032f) < 0.00001f, "Rotated right eye");
	ViewSettings narrow;
	narrow.outerFov = {{{-0.1f, 0.1f, 0.1f, -0.1f}, {-0.1f, 0.1f, 0.1f, -0.1f}}};
	narrow.insetWidth = 0;
	Require(ViewConfiguration(ViewLayout::Stereo, narrow).Describe().size() == 2, "Stereo depends on unused inset settings");
	ViewSettings settings;
	settings.outerFov[0] = {-1.1f, 0.8f, 1.0f, -0.7f};
	ViewConfiguration quad(ViewLayout::StereoWithInset, settings);
	Require(quad.Describe().size() == 4 && quad.EyeCount() == 2, "View count differs from eye count");
	views = quad.Locate(snapshot, false);
	Require(!views.gazeActive && views.views[0].description.eye == 0 && views.views[1].description.eye == 1 && views.views[2].description.eye == 0 && views.views[3].description.eye == 1, "VARJO view order");
	Require(views.views[2].description.role == ViewRole::Inset && views.views[2].description.width == 720, "Inset dimensions");
	Require(std::abs(views.views[2].fov.angleLeft + settings.insetHalfAngle) < 0.00001f, "Fixed inset field");
	state.gaze.yaw = 0.25f;
	state.gaze.pitch = 0.15f;
	snapshot = tracking.Publish({Duration{200}}, state);
	views = quad.Locate(snapshot, true);
	Require(views.gazeActive, "Active gaze foveation");
	Require(std::abs((views.views[2].fov.angleLeft + views.views[2].fov.angleRight) / 2 + 0.25f) < 0.00001f, "Gaze yaw convention");
	Require((views.views[2].fov.angleUp + views.views[2].fov.angleDown) > 0, "Gaze pitch convention");
	auto fixed = quad.Locate(snapshot, false);
	Require(std::abs(fixed.views[2].fov.angleLeft + settings.insetHalfAngle) < 0.00001f, "Per-call foveation activation");
	state.gaze.yaw = 1.4f;
	snapshot = tracking.Publish({Duration{300}}, state);
	views = quad.Locate(snapshot, true);
	Require(views.views[2].fov.angleLeft >= settings.outerFov[0].angleLeft && views.views[2].fov.angleRight <= settings.outerFov[0].angleRight, "Inset clipping");
	state.gaze.available = false;
	snapshot = tracking.Publish({Duration{400}}, state);
	views = quad.Locate(snapshot, true);
	Require(!views.gazeActive && std::abs(views.views[2].fov.angleLeft + settings.insetHalfAngle) < 0.00001f, "Gaze loss fixed fallback");
	snapshot.head.flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	views = stereo.Locate(snapshot);
	Require(views.views[0].pose.flags == 0, "Eye translation requires head orientation");
	std::cout << "View configuration tests passed\n";
}
