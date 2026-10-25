#include "simulation/BodyModel.h"
#include "math/Math.h"
#include "../TestSupport.h"

#include <cmath>
#include <iostream>
#include <limits>
#include <set>

static void FiniteBodyOutput()
{
	using namespace simxr;
	BodyModel model;
	PoseSample head{IdentityPose(), TrackedPoseFlags};
	head.pose.position.y = 1.7f;
	const float tiny = std::numeric_limits<float>::min() / 20;
	BodyModel tinyModel({tiny, 0.4f, 0.3f});
	const auto scaled = tinyModel.Evaluate(head, {}, {Duration{1}});
	for (const auto& joint : scaled.joints)
		Require(IsFinite(joint.pose.position), "Finite body geometry overflowed its height ratio");
	Require(std::abs(scaled.joints[static_cast<std::size_t>(BodyJoint::Hips)].pose.position.y - 1.7f * 0.53f) <
				0.00001f,
			"Small body proportions lost their relative hip height");
	Require(scaled.joints[static_cast<std::size_t>(BodyJoint::LeftFootAnkle)].pose.position.y == 0.08f,
			"Height scaling changed the fixed foot height");
	tinyModel.SetProportions({tiny / 16, 0.4f, 0.3f});
	bool rejected = false;
	try
	{
		(void)tinyModel.Evaluate(head, {}, {Duration{2}});
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Unrepresentable scaled body position was accepted");
	BodyControls controls;
	controls.controls[static_cast<std::size_t>(BodyControl::Hips)].offset = XrVector3f{-2e38f, 1, 0};
	controls.controls[static_cast<std::size_t>(BodyControl::Chest)].offset = XrVector3f{2e38f, 1, 0};
	const auto sample = model.Evaluate(head, controls, {Duration{1}});
	for (const auto& joint : sample.joints)
		Require(std::isfinite(joint.pose.position.x) && std::isfinite(joint.pose.position.y) &&
					std::isfinite(joint.pose.position.z),
				"Finite body controls overflowed interpolation");
	controls = {};
	head.pose.position.x = 2e38f;
	controls.joints[static_cast<std::size_t>(BodyJoint::Head)] =
		BodyJointOverride{XrPosef{{0, 0, 0, 1}, {2e38f, 1.7f, 0}}, true};
	rejected = false;
	try
	{
		(void)model.Evaluate(head, controls, {Duration{2}});
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Unrepresentable generated body position was accepted");
}

int main()
{
	FiniteBodyOutput();
	using namespace simxr;
	BodyModel body;
	Require(BodyModel::JointMap(BodyJointSet::UpperBodyFB).size() == XR_BODY_JOINT_COUNT_FB, "FB schema count");
	Require(BodyModel::JointMap(BodyJointSet::FullBodyMETA).size() == XR_FULL_BODY_JOINT_COUNT_META, "META schema count");
	Require(BodyModel::JointMap(BodyJointSet::WithoutArmsBD).size() == XR_BODY_JOINT_WITHOUT_ARM_COUNT_BD, "BD reduced schema count");
	Require(BodyModel::JointMap(BodyJointSet::FullBodyBD).size() == XR_BODY_JOINT_COUNT_BD, "BD schema count");
	Require(BodyModel::JointMap(BodyJointSet::UpperBodyFB)[XR_BODY_JOINT_HEAD_FB] == BodyJoint::Head, "FB head mapping");
	Require(BodyModel::JointMap(BodyJointSet::FullBodyMETA)[XR_FULL_BODY_JOINT_LEFT_FOOT_BALL_META] == BodyJoint::LeftFootBall, "META foot mapping");
	Require(BodyModel::JointMap(BodyJointSet::FullBodyBD)[XR_BODY_JOINT_LEFT_KNEE_BD] == BodyJoint::LeftLowerLeg, "BD knee mapping");
	Require(BodyModel::JointMap(BodyJointSet::WithoutArmsBD)[XR_BODY_JOINT_HEAD_BD] == BodyJoint::Head, "BD reduced head");
	for (auto set : {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA, BodyJointSet::WithoutArmsBD, BodyJointSet::FullBodyBD})
	{
		auto mapping = BodyModel::JointMap(set);
		Require(std::set<BodyJoint>(mapping.begin(), mapping.end()).size() == mapping.size(), "Aliased anatomical joints");
	}
	auto skeleton = body.Skeleton();
	Require(skeleton.size() == BodyJointCount, "Skeleton size");
	for (std::size_t index = 0; index < skeleton.size(); ++index)
	{
		Require(static_cast<std::size_t>(skeleton[index].joint) == index, "Canonical skeleton indexing");
		std::set<BodyJoint> visited;
		for (auto parent = skeleton[index].parent; parent; parent = skeleton[static_cast<std::size_t>(*parent)].parent)
			Require(visited.insert(*parent).second && *parent != skeleton[index].joint, "Skeleton parent cycle");
	}
	PoseSample head{{EulerAngles(0, 0.2f, 0), {3, 1.7f, 4}}, TrackedPoseFlags};
	BodyControls controls;
	auto sample = body.Evaluate(head, controls, {Duration{100}});
	auto index = [](BodyJoint joint) { return static_cast<std::size_t>(joint); };
	Require(sample.active && sample.confidence == 1 && sample.skeletonRevision == 1, "Active body status");
	Require(sample.joints[index(BodyJoint::Root)].pose.position.y == 0, "Root floor");
	Require(std::abs(sample.joints[index(BodyJoint::Hips)].pose.position.y - 1.7f * 0.53f) < 0.00001f, "Legacy hip proportion");
	Require(std::abs(sample.joints[index(BodyJoint::Chest)].pose.position.y - 1.7f * 0.76f) < 0.00001f, "Legacy chest proportion");
	Require(sample.joints[index(BodyJoint::LeftHandIndexTip)].pose.position.y != sample.joints[index(BodyJoint::Chest)].pose.position.y, "Hands collapsed into chest");
	controls.controls[static_cast<std::size_t>(BodyControl::LeftFoot)] = {XrVector3f{-0.15f, 0.1f, -0.3f}, false};
	head.pose.orientation = EulerAngles(1.5707963268f, 0, 0);
	sample = body.Evaluate(head, controls, {Duration{200}});
	auto foot = sample.joints[index(BodyJoint::LeftFootAnkle)];
	Require(std::abs(foot.pose.position.x - 2.7f) < 0.00001f && std::abs(foot.pose.position.z - 4.15f) < 0.00001f, "Legacy yaw-frame offset");
	Require(std::abs(foot.pose.position.y - 0.1f) < 0.00001f && foot.flags == ValidPoseFlags, "Inferred foot remains valid");
	controls.active = false;
	sample = body.Evaluate(head, controls, {Duration{300}});
	Require(!sample.active && sample.confidence == 0, "Inactive body");
	for (const auto& joint : sample.joints)
		Require(joint.flags == 0, "Inactive body exposes valid joints");
	controls = {};
	controls.preset = BodyPreset::TPose;
	auto tPose = body.Evaluate(head, controls, {Duration{400}});
	Require(std::abs(tPose.joints[index(BodyJoint::LeftHandPalm)].pose.position.y - tPose.joints[index(BodyJoint::LeftShoulder)].pose.position.y) < 0.00001f, "T pose arm elevation");
	controls.joints[index(BodyJoint::Head)] = BodyJointOverride{XrPosef{EulerAngles(0, 0, 0), {0.1f, 1.8f, 0}}, false};
	auto overridden = body.Evaluate(head, controls, {Duration{500}});
	Require(overridden.joints[index(BodyJoint::Head)].flags == ValidPoseFlags && std::abs(overridden.joints[index(BodyJoint::Head)].pose.position.y - 1.8f) < 0.00001f, "Joint override");
	auto proportions = body.Proportions();
	body.SetProportions(proportions);
	Require(body.Revision() == 1, "Unchanged skeleton revision");
	proportions.shoulderWidth = 0.45f;
	body.SetProportions(proportions);
	Require(body.Revision() == 2 && sample.skeletonRevision == 1, "Retained skeleton revision changed");
	Require(BodyModel::TrackerPersistentPath({9}) == "/user/vive_tracker_htcx/serial/simxr_0000000000000009", "Persistent tracker path");
	Require(BodyModel::TrackerRolePath(TrackerRole::Waist) == "/user/vive_tracker_htcx/role/waist", "Tracker role path");
	auto invalidHead = head;
	invalidHead.pose.orientation.w = 2;
	bool rejected = false;
	try
	{
		body.Evaluate(invalidHead, controls, {Duration{600}});
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Body accepts invalid head orientation");
	controls = {};
	controls.rootPose = XrPosef{EulerAngles(.4f, 0, 0), {2, .1f, -3}};
	head.pose = {EulerAngles(1.2f, .2f, 0), {2.2f, 1.8f, -3.1f}};
	const auto rooted = body.Evaluate(head, controls, {Duration{700}});
	const auto root = rooted.joints[index(BodyJoint::Root)].pose;
	Require(std::abs(root.position.x - 2) < .0001f && std::abs(root.position.y - .1f) < .0001f && std::abs(root.position.z + 3) < .0001f,
		"Body root did not retain independent floor position");
	Require(std::abs(RelativePose(root, *controls.rootPose).orientation.w) > .99999f, "Head look rotated body root");
	const auto bodyHead = rooted.joints[index(BodyJoint::Head)].pose;
	Require(std::abs(bodyHead.position.x - head.pose.position.x) < .0001f && std::abs(bodyHead.position.y - head.pose.position.y) < .0001f,
		"Body head lost the actual head pose during free look");
	std::cout << "Body model tests passed\n";
}
