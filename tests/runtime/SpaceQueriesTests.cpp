#include "runtime/SpaceQueries.h"
#include "math/Math.h"
#include "../TestSupport.h"

#include <array>
#include <cmath>
#include <iostream>

namespace
{
bool Near(float a, float b)
{
	return std::abs(a - b) < 0.00001f;
}
}

namespace
{
void FiniteViewQueries()
{
	using namespace simxr;
	TrackingSystem tracking;
	TrackingState input;
	input.head.pose.position.x = 2e38f;
	const auto snapshot = tracking.Publish({Duration{1000000}}, input);
	SpaceSystem spaces(reinterpret_cast<XrSession>(803), tracking);
	XrSpace base{};
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, {{0, 0, 0, 1}, {-2e38f, 0, 0}}, base) ==
				XR_SUCCESS,
			"Create distant view base");
	XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, 1000000, base};
	XrViewState state{XR_TYPE_VIEW_STATE};
	std::uint32_t count = 0;
	std::array<XrView, 2> views{{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}}};
	for (auto capacity : {0u, 2u})
		Require(LocateViews(spaces, ViewConfiguration{}, snapshot, info, &state, capacity, &count, views.data()) ==
						XR_SUCCESS &&
					!(state.viewStateFlags & (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_POSITION_TRACKED_BIT)) &&
					(state.viewStateFlags & XR_VIEW_STATE_ORIENTATION_VALID_BIT),
				"View relative overflow marked valid");
}
} // namespace

int main()
{
	FiniteViewQueries();
	using namespace simxr;
	TrackingSystem tracking;
	TrackingState input;
	input.head.pose = {EulerAngles(0.3f, 0, 0), {1, 1.7f, 2}};
	RuntimeTime time{Duration{1000000000}};
	auto snapshot = tracking.Publish(time, input);
	SpaceSystem spaces(reinterpret_cast<XrSession>(1), tracking);
	XrSpace view{}, base{};
	auto basePose = XrPosef{EulerAngles(1.5707963268f, 0, 0), {0.5f, 0, 1}};
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, IdentityPose(), view) == XR_SUCCESS, "Create view");
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, basePose, base) == XR_SUCCESS, "Create base");
	XrSpaceLocation location{XR_TYPE_SPACE_LOCATION};
	Require(LocateSpace(spaces, view, base, time.value.count(), &location) == XR_SUCCESS, "Locate space ABI");
	auto expected = RelativePose(snapshot.head.pose, basePose);
	Require(Near(location.pose.position.x, expected.position.x) && Near(location.pose.position.z, expected.position.z), "ABI rotated base");
	Require(LocateSpace(spaces, view, base, time.value.count(), nullptr) == XR_ERROR_VALIDATION_FAILURE, "Missing space output");
	location.type = XR_TYPE_VIEW;
	Require(LocateSpace(spaces, view, base, time.value.count(), &location) == XR_ERROR_VALIDATION_FAILURE, "Wrong location type");
	location.type = XR_TYPE_SPACE_LOCATION;
	XrBaseOutStructure unknownTail{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT, nullptr};
	XrSpaceVelocity velocity{XR_TYPE_SPACE_VELOCITY, &unknownTail};
	XrBaseOutStructure unknown{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT, reinterpret_cast<XrBaseOutStructure*>(&velocity)};
	location.next = &unknown;
	Require(LocateSpace(spaces, view, base, time.value.count(), &location) == XR_SUCCESS, "Unknown chain entries not ignored");
	Require(location.next == &unknown && unknown.next == reinterpret_cast<XrBaseOutStructure*>(&velocity) && velocity.next == &unknownTail && unknownTail.next == nullptr, "Space output chain pointers overwritten");
	Require(velocity.type == XR_TYPE_SPACE_VELOCITY && unknown.type == XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT, "Space output types overwritten");
	XrSpaceVelocity duplicate{XR_TYPE_SPACE_VELOCITY};
	velocity.next = &duplicate;
	Require(LocateSpace(spaces, view, base, time.value.count(), &location) == XR_ERROR_VALIDATION_FAILURE, "Duplicate velocity chain accepted");
	velocity.next = &unknownTail;
	unknownTail.next = &unknown;
	Require(LocateSpace(spaces, view, base, time.value.count(), &location) == XR_ERROR_VALIDATION_FAILURE, "Cyclic space output accepted");
	unknownTail.next = nullptr;
	location.next = nullptr;
	ViewConfiguration stereo;
	XrViewLocateInfo info{XR_TYPE_VIEW_LOCATE_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO, time.value.count(), base};
	XrViewState state{XR_TYPE_VIEW_STATE};
	std::uint32_t count = 999;
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 0, &count, nullptr) == XR_SUCCESS && count == 2, "Count-only stereo query");
	Require(LocateViews(spaces, stereo, snapshot, info, nullptr, 0, &count, nullptr) == XR_ERROR_VALIDATION_FAILURE, "Missing view state");
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 0, nullptr, nullptr) == XR_ERROR_VALIDATION_FAILURE, "Missing count");
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 2, &count, nullptr) == XR_ERROR_VALIDATION_FAILURE, "Missing view array");
	std::array<XrView, 4> views{};
	for (auto& value : views)
		value.type = XR_TYPE_VIEW;
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 1, &count, views.data()) == XR_ERROR_SIZE_INSUFFICIENT && count == 2, "Small stereo capacity");
	views[1].type = XR_TYPE_SPACE_LOCATION;
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 2, &count, views.data()) == XR_ERROR_VALIDATION_FAILURE, "Invalid view output type");
	views[1].type = XR_TYPE_VIEW;
	views[0].next = &unknownTail;
	state.next = &unknown;
	info.next = &unknownTail;
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 2, &count, views.data()) == XR_SUCCESS, "Stereo output");
	Require(state.next == &unknown && info.next == &unknownTail && views[0].next == &unknownTail, "View chain pointers overwritten");
	XrBaseOutStructure cycle{XR_TYPE_SYSTEM_EYE_GAZE_INTERACTION_PROPERTIES_EXT, nullptr};
	cycle.next = &cycle;
	info.next = &cycle;
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 2, &count, views.data()) == XR_ERROR_VALIDATION_FAILURE, "Cyclic view input chain accepted");
	info.next = &unknownTail;
	state.next = &cycle;
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 2, &count, views.data()) == XR_ERROR_VALIDATION_FAILURE, "Cyclic view state chain accepted");
	state.next = &unknown;
	views[0].next = &cycle;
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 2, &count, views.data()) == XR_ERROR_VALIDATION_FAILURE, "Cyclic individual view chain accepted");
	views[0].next = &unknownTail;
	auto eyeSamples = stereo.Locate(snapshot);
	expected = RelativePose(eyeSamples.views[0].pose.pose, basePose);
	Require(Near(views[0].pose.position.x, expected.position.x) && Near(views[0].pose.position.z, expected.position.z), "ABI eye/base transform");
	Require((state.viewStateFlags & (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT)) == (XR_VIEW_STATE_POSITION_VALID_BIT | XR_VIEW_STATE_ORIENTATION_VALID_BIT), "Valid view flags");
	snapshot.head.flags = XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 2, &count, views.data()) == XR_SUCCESS && state.viewStateFlags == 0, "IPD needs head orientation");
	snapshot.head.flags = ValidPoseFlags | XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	Require(LocateViews(spaces, stereo, snapshot, info, &state, 2, &count, views.data()) == XR_SUCCESS && !(state.viewStateFlags & XR_VIEW_STATE_POSITION_TRACKED_BIT) && (state.viewStateFlags & XR_VIEW_STATE_POSITION_VALID_BIT), "Inferred head orientation eye flags");
	snapshot.head.flags = TrackedPoseFlags;
	ViewConfiguration quad(ViewLayout::StereoWithInset);
	info.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_QUAD_VARJO;
	Require(LocateViews(spaces, quad, snapshot, info, &state, 0, &count, nullptr) == XR_SUCCESS && count == 4, "Count-only four-view query");
	Require(LocateViews(spaces, quad, snapshot, info, &state, 3, &count, views.data()) == XR_ERROR_SIZE_INSUFFICIENT && count == 4, "Four-view capacity");
	Require(LocateViews(spaces, quad, snapshot, info, &state, 4, &count, views.data()) == XR_SUCCESS && count == 4, "Four-view output");
	Require(Near(views[0].pose.position.x, views[2].pose.position.x) && Near(views[1].pose.position.x, views[3].pose.position.x) && views[2].fov.angleRight < views[0].fov.angleRight, "Four views represent paired eyes/insets");
	const std::array eyeExtension{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME};
	EnabledExtensions eyeEnabled(eyeExtension);
	const auto gazeAction = reinterpret_cast<XrAction>(99);
	const auto inferredBaseAction = reinterpret_cast<XrAction>(98);
	SpaceSystem eyeSpaces(reinterpret_cast<XrSession>(5), tracking,
		[&](XrAction action, XrPath, RuntimeTime requested)
		{
			if (action == inferredBaseAction)
				return PoseSample{IdentityPose(), ValidPoseFlags | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT, requested};
			auto history = tracking.Sample(requested);
			return history ? history->gaze : PoseSample{};
		}, [](XrAction, XrPath) { return XR_SUCCESS; });
	XrSpace eye{}, eyeBase{}, nongaze{}, inferredBase{};
	Require(eyeSpaces.CreateActionSpace(gazeAction, XR_NULL_PATH, {{0, 0, 0, 1}, {0.1f, 0, 0}}, eye) == XR_SUCCESS &&
		eyeSpaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, basePose, eyeBase) == XR_SUCCESS &&
		eyeSpaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_VIEW, IdentityPose(), nongaze) == XR_SUCCESS &&
		eyeSpaces.CreateActionSpace(inferredBaseAction, XR_NULL_PATH, IdentityPose(), inferredBase) == XR_SUCCESS, "Gaze ABI spaces");
	input.gaze.yaw = 0.4f;
	tracking.Publish({Duration{2000000000}}, input);
	XrEyeGazeSampleTimeEXT gazeTime{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT, &unknownTail, -42};
	velocity.next = &gazeTime;
	location.next = &velocity;
	Require(LocateSpace(eyeSpaces, eye, eyeBase, 1500000000, &location, &eyeEnabled) == XR_SUCCESS && gazeTime.time == 1500000000,
		"Interpolated gaze sample-time ABI output");
	Require(location.next == &velocity && velocity.next == &gazeTime && gazeTime.next == &unknownTail, "Gaze/velocity chain pointers overwritten");
	Require(LocateSpace(eyeSpaces, eye, eyeBase, 2500000000, &location, &eyeEnabled) == XR_SUCCESS && gazeTime.time == 2000000000,
		"Clamped gaze sample-time ABI output");
	Require(LocateSpace(eyeSpaces, eye, inferredBase, 1500000000, &location, &eyeEnabled) == XR_SUCCESS &&
		!(location.locationFlags & (XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT)), "Relative gaze tracking bits uncoupled");
	gazeTime.time = -42;
	Require(LocateSpace(eyeSpaces, nongaze, eyeBase, 1500000000, &location, &eyeEnabled) == XR_SUCCESS && gazeTime.time == 0,
		"Nongaze sample time not zeroed");
	gazeTime.time = -42;
	Require(LocateSpace(eyeSpaces, eye, eyeBase, 1, &location, &eyeEnabled) == XR_SUCCESS && gazeTime.time == 0,
		"Missing historical gaze sample time not zeroed");
	XrEyeGazeSampleTimeEXT duplicateTime{XR_TYPE_EYE_GAZE_SAMPLE_TIME_EXT, nullptr, 55};
	gazeTime.next = &duplicateTime;
	gazeTime.time = 77;
	Require(LocateSpace(eyeSpaces, eye, eyeBase, 1500000000, &location, &eyeEnabled) == XR_ERROR_VALIDATION_FAILURE && gazeTime.time == 77,
		"Duplicate enabled gaze-time chain");
	Require(LocateSpace(eyeSpaces, eye, eyeBase, 1500000000, &location) == XR_SUCCESS && gazeTime.time == 77 && duplicateTime.time == 55,
		"Disabled gaze structures consumed");
	gazeTime.next = &unknownTail;
	input.gaze.available = false;
	tracking.Publish({Duration{3000000000}}, input);
	Require(LocateSpace(eyeSpaces, eye, eyeBase, 3000000000, &location, &eyeEnabled) == XR_SUCCESS && gazeTime.time == 0 && !location.locationFlags,
		"Unavailable gaze sample time not zeroed");
	std::cout << "Space query ABI tests passed\n";
}
