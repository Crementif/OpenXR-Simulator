#include "runtime/extensions/HandTracking.h"
#include "math/Math.h"
#include "../../TestSupport.h"

#include <algorithm>
#include <array>
#include <cmath>
#include <iostream>
#include <numbers>

namespace
{
using namespace simxr;

constexpr XrTime Now = 1000000;

float BendDegrees(const std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT>& joints, XrHandJointEXT before,
				  XrHandJointEXT joint, XrHandJointEXT after)
{
	auto direction = [&](XrHandJointEXT from, XrHandJointEXT to) {
		const auto a = joints[from].pose.position, b = joints[to].pose.position;
		const XrVector3f d{b.x - a.x, b.y - a.y, b.z - a.z};
		const float length = std::sqrt(d.x * d.x + d.y * d.y + d.z * d.z);
		return XrVector3f{d.x / length, d.y / length, d.z / length};
	};
	const auto first = direction(before, joint), second = direction(joint, after);
	const float dot = first.x * second.x + first.y * second.y + first.z * second.z;
	return std::acos(std::clamp(dot, -1.0f, 1.0f)) * 180 / std::numbers::pi_v<float>;
}

struct Fixture
{
	TrackingSystem tracking;
	InputState input;
	SpaceSystem spaces{reinterpret_cast<XrSession>(901), tracking};
	XrSpace base{};

	Fixture()
	{
		tracking.Publish({Duration{Now}}, TrackingState{});
		Require(spaces.CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, IdentityPose(), base) == XR_SUCCESS,
				"Create hand base space");
	}

	void SetControls(std::size_t side, ControllerControls controls)
	{
		auto snapshot = input.Snapshot();
		snapshot.controllers[side] = controls;
		Require(input.Publish(snapshot) == XR_SUCCESS, "Publish controls");
	}
};

void LocatesControllerDrivenHands()
{
	const std::array extensions{XR_EXT_HAND_TRACKING_EXTENSION_NAME, XR_EXT_HAND_TRACKING_DATA_SOURCE_EXTENSION_NAME};
	Fixture fixture;
	HandTracking hands(reinterpret_cast<XrSession>(901), fixture.tracking, fixture.input, fixture.spaces,
					   EnabledExtensions(extensions));

	XrHandTrackerCreateInfoEXT create{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, nullptr, XR_HAND_RIGHT_EXT,
									  XR_HAND_JOINT_SET_DEFAULT_EXT};
	XrHandTrackerEXT right{};
	Require(hands.Create(&create, &right) == XR_SUCCESS, "Create right hand tracker");
	create.hand = static_cast<XrHandEXT>(3);
	XrHandTrackerEXT invalid{};
	Require(hands.Create(&create, &invalid) == XR_ERROR_VALIDATION_FAILURE, "Invalid hand accepted");

	std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
	std::array<XrHandJointVelocityEXT, XR_HAND_JOINT_COUNT_EXT> velocities{};
	XrHandTrackingDataSourceStateEXT source{XR_TYPE_HAND_TRACKING_DATA_SOURCE_STATE_EXT};
	XrHandJointVelocitiesEXT velocityChain{XR_TYPE_HAND_JOINT_VELOCITIES_EXT, &source, XR_HAND_JOINT_COUNT_EXT,
										   velocities.data()};
	XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, &velocityChain, XR_FALSE,
									  XR_HAND_JOINT_COUNT_EXT, joints.data()};
	XrHandJointsLocateInfoEXT locate{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, nullptr, fixture.base, Now};

	fixture.SetControls(1, {});
	Require(hands.Locate(right, &locate, &locations) == XR_SUCCESS && locations.isActive && source.isActive &&
				source.dataSource == XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT,
			"Controller-driven hand inactive");
	for (const auto& joint : joints)
		Require((joint.locationFlags & XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
					(joint.locationFlags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) && joint.radius > 0,
				"Joint not located");
	const float openIndex = BendDegrees(joints, XR_HAND_JOINT_INDEX_PROXIMAL_EXT, XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT,
										XR_HAND_JOINT_INDEX_DISTAL_EXT);
	const float openMiddle = BendDegrees(joints, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT,
										 XR_HAND_JOINT_MIDDLE_INTERMEDIATE_EXT, XR_HAND_JOINT_MIDDLE_DISTAL_EXT);
	Require(openIndex < 1 && openMiddle > 5 && openMiddle < 30, "Open hand curls");

	ControllerControls fist;
	fist.trigger = 1;
	fist.triggerTouch = true;
	fist.grip = 1;
	fist.thumbrestTouch = true;
	fixture.SetControls(1, fist);
	Require(hands.Locate(right, &locate, &locations) == XR_SUCCESS && locations.isActive, "Locate fist");
	Require(BendDegrees(joints, XR_HAND_JOINT_INDEX_PROXIMAL_EXT, XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT,
						XR_HAND_JOINT_INDEX_DISTAL_EXT) > 90 &&
				BendDegrees(joints, XR_HAND_JOINT_LITTLE_PROXIMAL_EXT, XR_HAND_JOINT_LITTLE_INTERMEDIATE_EXT,
							XR_HAND_JOINT_LITTLE_DISTAL_EXT) > 90 &&
				BendDegrees(joints, XR_HAND_JOINT_THUMB_PROXIMAL_EXT, XR_HAND_JOINT_THUMB_DISTAL_EXT,
							XR_HAND_JOINT_THUMB_TIP_EXT) > 30,
			"Fist curls");

	auto pointsAlong = [&](XrHandJointEXT joint, XrHandJointEXT tip) {
		const auto z = Rotate(joints[joint].pose.orientation, {0, 0, -1});
		const auto a = joints[joint].pose.position, b = joints[tip].pose.position;
		return z.x * (b.x - a.x) + z.y * (b.y - a.y) + z.z * (b.z - a.z) > 0;
	};
	Require(pointsAlong(XR_HAND_JOINT_INDEX_PROXIMAL_EXT, XR_HAND_JOINT_INDEX_INTERMEDIATE_EXT) &&
				pointsAlong(XR_HAND_JOINT_PALM_EXT, XR_HAND_JOINT_MIDDLE_PROXIMAL_EXT),
			"Joint -Z does not point towards the finger tip");

	ControllerControls disconnected;
	disconnected.connected = false;
	fixture.SetControls(1, disconnected);
	Require(hands.Locate(right, &locate, &locations) == XR_SUCCESS && !locations.isActive && !source.isActive &&
				joints.front().locationFlags == 0,
			"Disconnected controller still drives a hand");

	const std::array unobstructed{XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT};
	XrHandTrackingDataSourceInfoEXT sources{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT, nullptr, 1,
											const_cast<XrHandTrackingDataSourceEXT*>(unobstructed.data())};
	create = {XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, &sources, XR_HAND_LEFT_EXT, XR_HAND_JOINT_SET_DEFAULT_EXT};
	XrHandTrackerEXT optical{};
	Require(hands.Create(&create, &optical) == XR_SUCCESS, "Create unobstructed-only tracker");
	fixture.SetControls(0, {});
	Require(hands.Locate(optical, &locate, &locations) == XR_SUCCESS && !locations.isActive && !source.isActive,
			"Unobstructed-only tracker reported controller data");

	const std::array duplicate{XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT, XR_HAND_TRACKING_DATA_SOURCE_CONTROLLER_EXT};
	sources.requestedDataSourceCount = 2;
	sources.requestedDataSources = const_cast<XrHandTrackingDataSourceEXT*>(duplicate.data());
	Require(hands.Create(&create, &optical) == XR_ERROR_VALIDATION_FAILURE, "Duplicate data sources accepted");
	sources.requestedDataSourceCount = 0;
	Require(hands.Create(&create, &optical) == XR_ERROR_VALIDATION_FAILURE, "Empty data sources accepted");

	locations.jointCount = XR_HAND_JOINT_COUNT_EXT - 1;
	Require(hands.Locate(right, &locate, &locations) == XR_ERROR_VALIDATION_FAILURE, "Short joint array accepted");
	Require(hands.Destroy(right) == XR_SUCCESS && hands.Destroy(right) == XR_ERROR_HANDLE_INVALID,
			"Destroy hand tracker");
}

void RequiresExtension()
{
	Fixture fixture;
	HandTracking hands(reinterpret_cast<XrSession>(901), fixture.tracking, fixture.input, fixture.spaces,
					   EnabledExtensions{});
	XrHandTrackerCreateInfoEXT create{XR_TYPE_HAND_TRACKER_CREATE_INFO_EXT, nullptr, XR_HAND_LEFT_EXT,
									  XR_HAND_JOINT_SET_DEFAULT_EXT};
	XrHandTrackerEXT tracker{};
	Require(hands.Create(&create, &tracker) == XR_ERROR_FUNCTION_UNSUPPORTED, "Disabled extension created a tracker");

	const std::array plain{XR_EXT_HAND_TRACKING_EXTENSION_NAME};
	HandTracking plainHands(reinterpret_cast<XrSession>(901), fixture.tracking, fixture.input, fixture.spaces,
							EnabledExtensions(plain));
	const std::array unobstructed{XR_HAND_TRACKING_DATA_SOURCE_UNOBSTRUCTED_EXT};
	XrHandTrackingDataSourceInfoEXT sources{XR_TYPE_HAND_TRACKING_DATA_SOURCE_INFO_EXT, nullptr, 1,
											const_cast<XrHandTrackingDataSourceEXT*>(unobstructed.data())};
	create.next = &sources;
	Require(plainHands.Create(&create, &tracker) == XR_SUCCESS, "Create plain hand tracker");
	std::array<XrHandJointLocationEXT, XR_HAND_JOINT_COUNT_EXT> joints{};
	XrHandJointLocationsEXT locations{XR_TYPE_HAND_JOINT_LOCATIONS_EXT, nullptr, XR_FALSE, XR_HAND_JOINT_COUNT_EXT,
									  joints.data()};
	XrHandJointsLocateInfoEXT locate{XR_TYPE_HAND_JOINTS_LOCATE_INFO_EXT, nullptr, fixture.base, Now};
	fixture.SetControls(0, {});
	Require(plainHands.Locate(tracker, &locate, &locations) == XR_SUCCESS && locations.isActive,
			"Data source chained without its extension was not ignored");
}
} // namespace

int main()
{
	LocatesControllerDrivenHands();
	RequiresExtension();
	std::cout << "Hand tracking tests passed\n";
}
