#include "../TestSupport.h"
#include "Pch.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include "runtime/SpaceQueries.h"
#include "simulation/CommandState.h"
#include "simulation/TrackingSystem.h"
#include "simulation/ViewConfiguration.h"

#include <cmath>
#include <iostream>

namespace simxr::tests
{
static void EyeCommands()
{
	CommandState state;
	CommandConfiguration baseline;
	StereoEyeViews eyes;
	eyes.spaceRevision = 17;
	eyes.poses[0].flags = TrackedPoseFlags;
	eyes.poses[0].pose.position.x = -.04f;
	eyes.poses[0].pose.orientation.w = .999999f;
	eyes.fovs[1] = {-.7f, .8f, .6f, -.9f};
	CommandTransaction transaction;
	transaction.source = {CommandSourceKind::Device, 1};
	transaction.commands = {SetEyeViews{eyes}};
	Require(state.Apply(transaction, baseline, {Duration{10}}) == CommandResult::Conflict,
			"Eye observations require Views ownership");
	transaction.commands.insert(transaction.commands.begin(), TakeControl{ControlChannel::Views});
	Require(state.Apply(transaction, baseline, {Duration{10}}) == CommandResult::Applied,
			"Claim and observed geometry apply atomically");
	const auto applied = state.Merge(baseline);
	Require(applied.tracking.eyeViews->spaceRevision == 17 &&
				applied.tracking.eyeViews->poses[0].pose.orientation.w == 1 && applied.views.ipd == baseline.views.ipd,
			"Observed eye command normalizes poses without replacing view settings");
	const auto encoded = control::EncodeSimulationCommand(transaction.commands.back(), 1);
	const auto decoded = std::get<SetEyeViews>(control::DecodeSimulationCommand(encoded));
	Require(decoded.value->poses[0].pose.position.x == -.04f && decoded.value->poses[1].flags == 0 &&
				decoded.value->fovs[1].angleDown == -.9f && decoded.value->spaceRevision == 17,
			"Wire eye command preserves geometry, invalidity and calibration revision");
	eyes.fovs[0].angleLeft = 2;
	transaction.commands = {SetEyeViews{eyes}};
	Require(state.Apply(transaction, baseline, {Duration{20}}) == CommandResult::Invalid &&
				state.Merge(baseline).tracking.eyeViews->poses[0].pose.position.x == -.04f,
			"Invalid eye geometry cannot partially replace the current view source");
	state.ReleaseSource(transaction.source);
	Require(!state.Merge(baseline).tracking.eyeViews, "Source release restores baseline views");
	transaction.commands = {TakeControl{ControlChannel::Views}, SetEyeViews{decoded.value}, SetEyeViews{}};
	Require(state.Apply(transaction, baseline, {Duration{30}}) == CommandResult::Applied &&
				!state.Merge(baseline).tracking.eyeViews,
			"Explicit clear restores simulated geometry while retaining ownership");
	Require(!std::get<SetEyeViews>(control::DecodeSimulationCommand(control::EncodeSimulationCommand(SetEyeViews{}, 1)))
				 .value,
			"Clear survives the command wire format");
}
static void PhysicalViews()
{
	TrackingSystem tracking;
	TrackingState state;
	state.head.flags = 0;
	state.eyeViews = StereoEyeViews{};
	state.eyeViews->spaceRevision = 1;
	for (std::size_t eye = 0; eye < 2; ++eye)
	{
		state.eyeViews->poses[eye].flags = TrackedPoseFlags;
		state.eyeViews->poses[eye].pose = {EulerAngles(eye ? -.15f : .15f, 0, 0), {eye ? .04f : -.03f, 1.6f, -.01f}};
	}
	state.eyeViews->fovs = {{{-.7f, .9f, .8f, -.6f}, {-.9f, .7f, .6f, -.8f}}};
	auto first = tracking.Publish({Duration{100}}, state);
	ViewConfiguration stereo;
	const auto located = stereo.Locate(first);
	Require(located.views[0].pose.flags == TrackedPoseFlags && located.views[0].pose.pose.position.x == -.03f &&
				located.views[1].pose.pose.position.x == .04f && located.views[0].fov.angleLeft == -.7f &&
				located.views[1].fov.angleUp == .6f && located.views[0].pose.pose.orientation.y > 0,
			"Observed eye origins, cant and asymmetric FOV survive invalid head tracking");
	SpaceSystem spaces(reinterpret_cast<XrSession>(1), tracking);
	Require(spaces.StartGeneration({Duration{1}}, {}) == XR_SUCCESS, "Start physical view reference origin");
	XrSpace local = XR_NULL_HANDLE;
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, IdentityPose(), local) == XR_SUCCESS,
			"Create application local space");
	XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.space = local;
	locate.displayTime = 100;
	XrViewState viewState{XR_TYPE_VIEW_STATE};
	std::array<XrView, 2> output{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
	std::uint32_t count = 0;
	Require(LocateViews(spaces, stereo, first, locate, &viewState, 2, &count, output.data()) == XR_SUCCESS &&
				count == 2 && (viewState.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT) &&
				output[0].pose.position.x == -.03f && output[1].fov.angleUp == .6f,
			"Application view query returns observed geometry without requiring valid head pose");
	state.eyeViews->poses[0].pose.position.x = .07f;
	tracking.Publish({Duration{200}}, state);
	const auto middle = stereo.Locate(*tracking.Sample({Duration{150}}));
	Require(std::abs(middle.views[0].pose.pose.position.x - .02f) < .00001f,
			"Continuous observed eyes interpolate in tracking history");
	state.eyeViews->spaceRevision = 2;
	state.eyeViews->poses[0].pose.position.x = 10;
	tracking.Publish({Duration{300}}, state);
	Require(stereo.Locate(*tracking.Sample({Duration{250}})).views[0].pose.pose.position.x == .07f,
			"Calibration revision changes do not blend unrelated eye spaces");
	state.eyeViews->poses[0].flags = 0;
	const auto lost = stereo.Locate(tracking.Publish({Duration{400}}, state));
	Require(lost.views[0].pose.flags == 0 && lost.views[1].pose.flags == TrackedPoseFlags,
			"Lost physical eye does not fabricate a head-derived fallback or invalidate its peer");
	state.eyeViews->fovs[0] = {-.1f, .1f, .1f, -.1f};
	const auto narrow = ViewConfiguration(ViewLayout::StereoWithInset).Locate(tracking.Publish({Duration{500}}, state));
	Require(narrow.views[2].fov.angleLeft >= -.1f && narrow.views[2].fov.angleRight <= .1f &&
				narrow.views[2].fov.angleLeft < narrow.views[2].fov.angleRight,
			"Inset remains inside a narrow physical FOV");
	state.eyeViews->fovs[0].angleLeft = 1.6f;
	bool rejected = false;
	try
	{
		tracking.Publish({Duration{600}}, state);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Reject invalid physical FOV before publishing history");
	state.eyeViews.reset();
	state.head.flags = TrackedPoseFlags;
	const auto fallback = stereo.Locate(tracking.Publish({Duration{600}}, state));
	Require(std::abs(fallback.views[0].pose.pose.position.x + .032f) < .00001f,
			"Explicit removal restores simulated eye geometry");
	Require(stereo.Locate(first).views[0].pose.pose.position.x == -.03f,
			"Later physical updates do not mutate retained snapshots");
}
} // namespace simxr::tests
int main()
{
	try
	{
		simxr::tests::PhysicalViews();
		simxr::tests::EyeCommands();
		std::cout << "Physical eye view checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
