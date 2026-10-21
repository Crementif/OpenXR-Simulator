#include "runtime/extensions/BodyTracking.h"
#include "Math.h"
#include "../../TestSupport.h"

#include <array>
#include <atomic>
#include <cmath>
#include <iostream>
#include <limits>
#include <thread>

namespace
{
bool Near(float a, float b)
{
	return std::abs(a - b) < 0.00001f;
}
}

namespace
{
void FiniteBodyQueries()
{
	using namespace simxr;
	const std::array extensions{XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_BD_BODY_TRACKING_EXTENSION_NAME};
	TrackingSystem tracking;
	TrackingState input;
	input.head.pose.position.x = 2e38f;
	tracking.Publish({Duration{1000000}}, input);
	SpaceSystem spaces(reinterpret_cast<XrSession>(802), tracking);
	XrSpace base{};
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, {{0, 0, 0, 1}, {-2e38f, 0, 0}}, base) ==
				XR_SUCCESS,
			"Create distant body base");
	BodyTracking bodies(reinterpret_cast<XrSession>(802), tracking, spaces, EnabledExtensions(extensions));
	XrBodyTrackerCreateInfoFB create{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB, nullptr, XR_BODY_JOINT_SET_DEFAULT_FB};
	XrBodyTrackerFB tracker{};
	Require(bodies.CreateFB(&create, &tracker) == XR_SUCCESS, "Create finite-limit body tracker");
	std::array<XrBodyJointLocationFB, XR_BODY_JOINT_COUNT_FB> joints{};
	XrBodyJointLocationsFB locations{XR_TYPE_BODY_JOINT_LOCATIONS_FB, nullptr,		XR_FALSE, 0,
									 XR_BODY_JOINT_COUNT_FB,		  joints.data()};
	XrBodyJointsLocateInfoFB locate{XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB, nullptr, base, 1000000};
	Require(bodies.LocateFB(tracker, &locate, &locations) == XR_SUCCESS, "Locate distant body joints");
	for (const auto& joint : joints)
		Require(
			!(joint.locationFlags & (XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT)) &&
				(joint.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT),
			"Invalid body relative position marked valid");
	XrBodyTrackerCreateInfoBD createBD{XR_TYPE_BODY_TRACKER_CREATE_INFO_BD, nullptr,
									   XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD};
	XrBodyTrackerBD trackerBD{};
	Require(bodies.CreateBD(&createBD, &trackerBD) == XR_SUCCESS, "Create BD finite-limit tracker");
	std::array<XrBodyJointLocationBD, XR_BODY_JOINT_COUNT_BD> jointsBD{};
	XrBodyJointLocationsBD locationsBD{XR_TYPE_BODY_JOINT_LOCATIONS_BD, nullptr, XR_FALSE, XR_BODY_JOINT_COUNT_BD,
									   jointsBD.data()};
	XrBodyJointsLocateInfoBD locateBD{XR_TYPE_BODY_JOINTS_LOCATE_INFO_BD, nullptr, base, 1000000};
	Require(bodies.LocateBD(trackerBD, &locateBD, &locationsBD) == XR_SUCCESS && !locationsBD.allJointPosesTracked,
			"BD unrepresentable joints reported fully tracked");
	for (const auto& joint : jointsBD)
		Require(!(joint.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT), "BD relative overflow marked valid");
	std::array<XrBodySkeletonJointFB, XR_BODY_JOINT_COUNT_FB> skeletonJoints{};
	skeletonJoints.front().joint = 99;
	XrBodySkeletonFB skeleton{XR_TYPE_BODY_SKELETON_FB, nullptr, XR_BODY_JOINT_COUNT_FB, skeletonJoints.data()};
	Require(bodies.SkeletonFB(tracker, &skeleton) == XR_ERROR_RUNTIME_FAILURE && skeletonJoints.front().joint == 99,
			"Unrepresentable skeleton published or partially changed output");
}
} // namespace

int main()
{
	FiniteBodyQueries();
	using namespace simxr;
	const std::array extensions{XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME,
		XR_BD_BODY_TRACKING_EXTENSION_NAME, XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME, XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME};
	EnabledExtensions enabled(extensions);
	TrackingSystem tracking;
	TrackingState input;
	input.head.pose = {EulerAngles(0.4f, 0.2f, 0), {1, 1.7f, 2}};
	RuntimeTime first{Duration{1000000000}}, last{Duration{2000000000}};
	auto original = tracking.Publish(first, input);
	input.head.pose.position.x = 3;
	tracking.Publish(last, input);
	auto session = reinterpret_cast<XrSession>(1);
	SpaceSystem spaces(session, tracking);
	XrSpace base{};
	auto basePose = XrPosef{EulerAngles(0.8f, 0, 0), {2, 0, 3}};
	Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_LOCAL, basePose, base) == XR_SUCCESS, "Create body base");
	BodyTracking bodies(session, tracking, spaces, enabled);
	XrBodyTrackerCreateInfoFB create{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB, nullptr, XR_BODY_JOINT_SET_DEFAULT_FB};
	XrBodyTrackerFB upper{}, full{};
	Require(bodies.CreateFB(&create, &upper) == XR_SUCCESS, "Create upper body");
	create.bodyJointSet = XR_BODY_JOINT_SET_FULL_BODY_META;
	Require(bodies.CreateFB(&create, &full) == XR_SUCCESS, "Create full body META");
	create.bodyJointSet = XR_BODY_JOINT_SET_MAX_ENUM_FB;
	XrBodyTrackerFB unchanged = full;
	Require(bodies.CreateFB(&create, &unchanged) == XR_ERROR_VALIDATION_FAILURE && unchanged == full, "Invalid FB set mutated output");
	XrBodyTrackerCreateInfoBD createBD{XR_TYPE_BODY_TRACKER_CREATE_INFO_BD, nullptr, XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD};
	XrBodyTrackerBD bd{}, reduced{};
	Require(bodies.CreateBD(&createBD, &bd) == XR_SUCCESS, "Create full BD body");
	createBD.jointSet = XR_BODY_JOINT_SET_BODY_WITHOUT_ARM_BD;
	Require(bodies.CreateBD(&createBD, &reduced) == XR_SUCCESS, "Create reduced BD body");
	std::array<XrBodyJointLocationFB, XR_FULL_BODY_JOINT_COUNT_META> joints{};
	XrBodyTrackingFidelityStatusMETA fidelity{XR_TYPE_BODY_TRACKING_FIDELITY_STATUS_META};
	XrBodyTrackingCalibrationStatusMETA calibration{XR_TYPE_BODY_TRACKING_CALIBRATION_STATUS_META, &fidelity};
	XrBodyJointLocationsFB locations{XR_TYPE_BODY_JOINT_LOCATIONS_FB, &calibration, XR_FALSE, 0, XR_BODY_JOINT_COUNT_FB, joints.data()};
	XrBodyJointsLocateInfoFB locate{XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB, nullptr, base, first.value.count()};
	Require(bodies.LocateFB(upper, &locate, &locations) == XR_SUCCESS && locations.isActive && locations.confidence == 1 && locations.time == first.value.count() && locations.skeletonChangedCount == 1, "FB activity/time/revision");
	auto expected = RelativePose(original.body.joints[static_cast<std::size_t>(BodyJoint::Head)].pose, basePose);
	Require(Near(joints[XR_BODY_JOINT_HEAD_FB].pose.position.x, expected.position.x) && Near(joints[XR_BODY_JOINT_HEAD_FB].pose.position.z, expected.position.z), "FB mapping/base transform");
	Require(calibration.status == XR_BODY_TRACKING_CALIBRATION_STATE_VALID_META && fidelity.fidelity == XR_BODY_TRACKING_FIDELITY_HIGH_META && locations.next == &calibration && calibration.next == &fidelity, "META status chain");
	locations.jointCount = XR_FULL_BODY_JOINT_COUNT_META;
	Require(bodies.LocateFB(upper, &locate, &locations) == XR_ERROR_VALIDATION_FAILURE, "Oversized FB count accepted");
	Require(bodies.LocateFB(full, &locate, &locations) == XR_SUCCESS && joints[XR_FULL_BODY_JOINT_LEFT_FOOT_BALL_META].locationFlags == TrackedPoseFlags, "META full mapping");
	locations.jointCount = 0;
	Require(bodies.LocateFB(full, &locate, &locations) == XR_ERROR_VALIDATION_FAILURE, "FB incorrectly accepts count-only query");
	locations.jointCount = XR_FULL_BODY_JOINT_COUNT_META;
	locations.jointLocations = nullptr;
	Require(bodies.LocateFB(full, &locate, &locations) == XR_ERROR_VALIDATION_FAILURE, "Null FB array");
	locations.jointLocations = joints.data();
	std::array<XrBodySkeletonJointFB, XR_FULL_BODY_JOINT_COUNT_META> skeletonJoints{};
	XrBodySkeletonFB skeleton{XR_TYPE_BODY_SKELETON_FB, nullptr, XR_FULL_BODY_JOINT_COUNT_META, skeletonJoints.data()};
	Require(bodies.SkeletonFB(full, &skeleton) == XR_SUCCESS, "Retrieve full body skeleton");
	Require(skeletonJoints[XR_FULL_BODY_JOINT_ROOT_META].parentJoint == XR_FULL_BODY_JOINT_NONE_META && skeletonJoints[XR_FULL_BODY_JOINT_HEAD_META].parentJoint == XR_FULL_BODY_JOINT_NECK_META, "META skeleton hierarchy");
	Require(!Near(skeletonJoints[XR_FULL_BODY_JOINT_LEFT_HAND_PALM_META].pose.position.x, skeletonJoints[XR_FULL_BODY_JOINT_CHEST_META].pose.position.x), "Skeleton lacks distinct T-pose hands");
	auto retainedSkeleton = skeletonJoints;
	XrBodyTrackingCalibrationInfoMETA height{XR_TYPE_BODY_TRACKING_CALIBRATION_INFO_META, nullptr, 2};
	Require(bodies.SuggestCalibrationFB(full, &height) == XR_SUCCESS, "Calibration height");
	Require(bodies.SkeletonFB(full, &skeleton) == XR_SUCCESS && Near(skeletonJoints[XR_FULL_BODY_JOINT_HEAD_META].pose.position.y, retainedSkeleton[XR_FULL_BODY_JOINT_HEAD_META].pose.position.y), "Calibration mutated retained query skeleton");
	Require(bodies.LocateFB(full, &locate, &locations) == XR_SUCCESS && locations.skeletonChangedCount == 2, "Calibration skeleton revision");
	Require(bodies.SkeletonFB(full, &skeleton) == XR_SUCCESS && Near(skeletonJoints[XR_FULL_BODY_JOINT_HEAD_META].pose.position.y, 2), "Calibration rest height");
	Require(bodies.SnapshotFB(upper)->heightOverride == std::nullopt, "Calibration leaked across tracker handles");
	Require(bodies.RequestFidelityFB(full, XR_BODY_TRACKING_FIDELITY_LOW_META) == XR_SUCCESS && bodies.SnapshotFB(full)->requestedFidelity == XR_BODY_TRACKING_FIDELITY_LOW_META, "Requested fidelity lost");
	Require(bodies.LocateFB(full, &locate, &locations) == XR_SUCCESS && fidelity.fidelity == XR_BODY_TRACKING_FIDELITY_HIGH_META, "Actual fidelity falsely reports request");
	Require(bodies.RequestFidelityFB(full, XR_BODY_TRACKING_FIDELITY_MAX_ENUM_META) == XR_ERROR_VALIDATION_FAILURE, "Invalid fidelity");
	for (float invalid : {0.5f, 3.0f, std::numeric_limits<float>::quiet_NaN()})
	{
		height.bodyHeight = invalid;
		Require(bodies.SuggestCalibrationFB(full, &height) == XR_ERROR_VALIDATION_FAILURE, "Invalid calibration height");
	}
	Require(bodies.ResetCalibrationFB(full) == XR_SUCCESS && !bodies.SnapshotFB(full)->heightOverride, "Calibration reset");
	Require(bodies.LocateFB(full, &locate, &locations) == XR_SUCCESS && locations.skeletonChangedCount == 3, "Reset skeleton revision");
	locate.time = 1500000000;
	Require(bodies.LocateFB(full, &locate, &locations) == XR_SUCCESS && locations.time == locate.time, "Interpolated body time");
	locate.time = 3000000000;
	Require(bodies.LocateFB(full, &locate, &locations) == XR_SUCCESS && locations.time == last.value.count(), "Future body sample time fabricated");
	locate.time = 1;
	Require(bodies.LocateFB(full, &locate, &locations) == XR_SUCCESS && !locations.isActive && locations.confidence == 0 && locations.time == 0, "Missing history active");
	for (const auto& joint : joints)
		Require(joint.locationFlags == 0, "Missing history valid joints");
	locate.time = 0;
	Require(bodies.LocateFB(full, &locate, &locations) == XR_ERROR_TIME_INVALID, "Invalid body time");
	locate.time = first.value.count();
	XrBodyTrackingCalibrationStatusMETA duplicate{XR_TYPE_BODY_TRACKING_CALIBRATION_STATUS_META};
	fidelity.next = &duplicate;
	Require(bodies.LocateFB(full, &locate, &locations) == XR_ERROR_VALIDATION_FAILURE, "Duplicate status chain");
	fidelity.next = &calibration;
	Require(bodies.LocateFB(full, &locate, &locations) == XR_ERROR_VALIDATION_FAILURE, "Cyclic status chain");
	fidelity.next = nullptr;
	std::array<XrBodyJointLocationBD, XR_BODY_JOINT_COUNT_BD> jointsBD{};
	XrBodyJointLocationsBD locationsBD{XR_TYPE_BODY_JOINT_LOCATIONS_BD, nullptr, XR_FALSE, XR_BODY_JOINT_COUNT_BD, jointsBD.data()};
	XrBodyJointsLocateInfoBD locateBD{XR_TYPE_BODY_JOINTS_LOCATE_INFO_BD, nullptr, base, first.value.count()};
	Require(bodies.LocateBD(bd, &locateBD, &locationsBD) == XR_SUCCESS && locationsBD.allJointPosesTracked, "BD all tracked");
	expected = RelativePose(original.body.joints[static_cast<std::size_t>(BodyJoint::LeftLowerLeg)].pose, basePose);
	Require(Near(jointsBD[XR_BODY_JOINT_LEFT_KNEE_BD].pose.position.x, expected.position.x), "Distinct BD mapping");
	Require(bodies.LocateBD(reduced, &locateBD, &locationsBD) == XR_ERROR_VALIDATION_FAILURE, "BD reduced count");
	locationsBD.jointLocationCount = XR_BODY_JOINT_WITHOUT_ARM_COUNT_BD;
	Require(bodies.LocateBD(reduced, &locateBD, &locationsBD) == XR_SUCCESS, "BD reduced output");
	input.body.controls[static_cast<std::size_t>(BodyControl::LeftFoot)].tracked = false;
	tracking.Publish({Duration{3000000000}}, input);
	locate.time = locateBD.time = 3000000000;
	Require(bodies.LocateFB(full, &locate, &locations) == XR_SUCCESS && locations.isActive && joints[XR_FULL_BODY_JOINT_LEFT_FOOT_BALL_META].locationFlags == ValidPoseFlags, "Inferred active FB joint");
	Require(bodies.LocateBD(reduced, &locateBD, &locationsBD) == XR_SUCCESS && !locationsBD.allJointPosesTracked && jointsBD[XR_BODY_JOINT_LEFT_FOOT_BD].locationFlags == ValidPoseFlags, "BD inferred joint");
	input.body.active = false;
	tracking.Publish({Duration{4000000000}}, input);
	locate.time = 4000000000;
	Require(bodies.LocateFB(full, &locate, &locations) == XR_SUCCESS && !locations.isActive, "Inactive FB body");
	for (const auto& joint : joints)
		Require(joint.locationFlags == 0, "Inactive FB valid flags");
	BodyTracking other(reinterpret_cast<XrSession>(9), tracking, spaces, enabled);
	Require(other.LocateFB(full, &locate, &locations) == XR_ERROR_HANDLE_INVALID, "Wrong-session body handle");
	Require(bodies.DestroyFB(reinterpret_cast<XrBodyTrackerFB>(bd)) == XR_ERROR_HANDLE_INVALID && bodies.DestroyBD(reinterpret_cast<XrBodyTrackerBD>(full)) == XR_ERROR_HANDLE_INVALID, "Wrong body handle family");
	EnabledExtensions noExtensions;
	BodyTracking disabled(session, tracking, spaces, noExtensions);
	create.bodyJointSet = XR_BODY_JOINT_SET_DEFAULT_FB;
	Require(disabled.CreateFB(&create, &unchanged) == XR_ERROR_FUNCTION_UNSUPPORTED && disabled.CreateBD(&createBD, &bd) == XR_ERROR_FUNCTION_UNSUPPORTED, "Disabled body feature");
	const std::array metaOnly{XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME};
	BodyTracking missingDependency(session, tracking, spaces, EnabledExtensions(metaOnly));
	Require(missingDependency.CreateFB(&create, &unchanged) == XR_ERROR_FUNCTION_UNSUPPORTED, "Missing FB dependency");
	BodyTrackingCapabilities unsupported;
	unsupported.fb = unsupported.bd = false;
	BodyTracking unavailable(session, tracking, spaces, enabled, unsupported);
	Require(unavailable.CreateFB(&create, &unchanged) == XR_ERROR_FEATURE_UNSUPPORTED && !unavailable.Capabilities().fullBodyMETA, "Unsupported feature capability");
	BodyTrackingCapabilities limits;
	limits.fullBodyMETA = limits.calibrationMETA = limits.fidelityMETA = false;
	BodyTracking limited(session, tracking, spaces, enabled, limits);
	XrBodyTrackerFB limitedUpper{};
	Require(limited.CreateFB(&create, &limitedUpper) == XR_SUCCESS, "Create capability-limited upper body");
	create.bodyJointSet = XR_BODY_JOINT_SET_FULL_BODY_META;
	Require(limited.CreateFB(&create, &unchanged) == XR_ERROR_FEATURE_UNSUPPORTED, "Unsupported full-body jointset");
	height.bodyHeight = 1.8f;
	Require(limited.SuggestCalibrationFB(limitedUpper, &height) == XR_ERROR_FEATURE_UNSUPPORTED && limited.ResetCalibrationFB(limitedUpper) == XR_ERROR_FEATURE_UNSUPPORTED &&
		limited.RequestFidelityFB(limitedUpper, XR_BODY_TRACKING_FIDELITY_LOW_META) == XR_ERROR_FEATURE_UNSUPPORTED, "Unsupported META controls");
	const std::array fbOnly{XR_FB_BODY_TRACKING_EXTENSION_NAME};
	BodyTracking justFB(session, tracking, spaces, EnabledExtensions(fbOnly));
	Require(justFB.CreateFB(&create, &unchanged) == XR_ERROR_VALIDATION_FAILURE, "Disabled full-body enumerant");
	create.bodyJointSet = XR_BODY_JOINT_SET_DEFAULT_FB;
	XrBodyTrackerFB plain{};
	Require(justFB.CreateFB(&create, &plain) == XR_SUCCESS, "FB without META");
	locations.jointCount = XR_BODY_JOINT_COUNT_FB;
	calibration.status = XR_BODY_TRACKING_CALIBRATION_STATE_MAX_ENUM_META;
	fidelity.fidelity = XR_BODY_TRACKING_FIDELITY_MAX_ENUM_META;
	Require(justFB.LocateFB(plain, &locate, &locations) == XR_SUCCESS && calibration.status == XR_BODY_TRACKING_CALIBRATION_STATE_MAX_ENUM_META &&
		fidelity.fidelity == XR_BODY_TRACKING_FIDELITY_MAX_ENUM_META, "Disabled META output nodes consumed");
	skeleton.jointCount = XR_BODY_JOINT_COUNT_FB;
	Require(bodies.SkeletonFB(upper, &skeleton) == XR_SUCCESS && skeletonJoints[XR_BODY_JOINT_ROOT_FB].parentJoint == XR_BODY_JOINT_NONE_FB, "FB skeleton parent schema");
	skeleton.jointCount = 0;
	Require(bodies.SkeletonFB(upper, &skeleton) == XR_ERROR_VALIDATION_FAILURE, "Skeleton count-only query accepted");
	skeleton.jointCount = XR_FULL_BODY_JOINT_COUNT_META;
	Require(bodies.SkeletonFB(full, &skeleton) == XR_SUCCESS, "Retained skeleton before later input");
	retainedSkeleton = skeletonJoints;
	auto proportions = original.proportions;
	proportions.shoulderWidth = 0.45f;
	tracking.SetBodyProportions(proportions);
	input.body.active = true;
	input.head.pose.position.x = 9;
	tracking.Publish({Duration{5000000000}}, input);
	Require(bodies.SkeletonFB(full, &skeleton) == XR_SUCCESS && Near(skeletonJoints[XR_FULL_BODY_JOINT_HEAD_META].pose.position.x, retainedSkeleton[XR_FULL_BODY_JOINT_HEAD_META].pose.position.x), "New input mutated retained body query");
	locate.time = 5000000000;
	Require(bodies.LocateFB(upper, &locate, &locations) == XR_SUCCESS && locations.skeletonChangedCount == 2, "Automatic proportions revision");
	std::atomic<bool> stop = false;
	std::thread observer([&]
	{
		while (!stop.load())
			bodies.SnapshotFB(full);
	});
	Require(bodies.DestroyFB(full) == XR_SUCCESS && bodies.DestroyFB(full) == XR_ERROR_HANDLE_INVALID, "Stale body handle");
	stop.store(true);
	observer.join();
	bodies.Clear();
	Require(!bodies.OwnsFB(upper) && !bodies.OwnsBD(bd) && !bodies.OwnsBD(reduced), "Parent body teardown");
	std::cout << "Body tracking tests passed\n";
}
