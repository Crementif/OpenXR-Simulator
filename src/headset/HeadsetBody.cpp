#include "headset/HeadsetBody.h"
#include "Pch.h"
#include "headset/OpenXrRuntime.h"
#include "math/Math.h"
#include "simulation/BodyModel.h"

namespace simxr::headset
{
HeadsetBody::HeadsetBody(OpenXrRuntime& runtime) : m_runtime(runtime) {}

HeadsetBody::~HeadsetBody()
{
	Close();
}

void HeadsetBody::Open(XrSession session, BodyJointSet jointSet)
{
	if (m_fb || m_bd || m_runtime.ResourcesPreserved())
		throw std::logic_error("Physical body provider cannot be reopened while native resources are retained");
	if (!session)
		throw std::invalid_argument("Physical body provider requires a session");
	const auto& support = m_runtime.BodyCapabilities();
	const bool fb = jointSet == BodyJointSet::UpperBodyFB || jointSet == BodyJointSet::FullBodyMETA;
	const bool bd = jointSet == BodyJointSet::WithoutArmsBD || jointSet == BodyJointSet::FullBodyBD;
	if ((!fb && !bd) || (fb && !support.fb) || (bd && !support.bd) ||
		(jointSet == BodyJointSet::FullBodyMETA && !support.fullBodyMETA))
		throw std::runtime_error("Selected physical body joint set is unavailable");
	if (fb)
	{
		const auto create = reinterpret_cast<PFN_xrCreateBodyTrackerFB>(m_runtime.Procedure("xrCreateBodyTrackerFB"));
		m_destroyFB = reinterpret_cast<PFN_xrDestroyBodyTrackerFB>(m_runtime.Procedure("xrDestroyBodyTrackerFB"));
		m_locateFB = reinterpret_cast<PFN_xrLocateBodyJointsFB>(m_runtime.Procedure("xrLocateBodyJointsFB"));
		m_skeletonFB = reinterpret_cast<PFN_xrGetBodySkeletonFB>(m_runtime.Procedure("xrGetBodySkeletonFB"));
		XrBodyTrackerCreateInfoFB info{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB};
		info.bodyJointSet =
			jointSet == BodyJointSet::FullBodyMETA ? XR_BODY_JOINT_SET_FULL_BODY_META : XR_BODY_JOINT_SET_DEFAULT_FB;
		CheckXr(create(session, &info, &m_fb), "Create physical FB body tracker");
	}
	else
	{
		const auto create = reinterpret_cast<PFN_xrCreateBodyTrackerBD>(m_runtime.Procedure("xrCreateBodyTrackerBD"));
		m_destroyBD = reinterpret_cast<PFN_xrDestroyBodyTrackerBD>(m_runtime.Procedure("xrDestroyBodyTrackerBD"));
		m_locateBD = reinterpret_cast<PFN_xrLocateBodyJointsBD>(m_runtime.Procedure("xrLocateBodyJointsBD"));
		XrBodyTrackerCreateInfoBD info{XR_TYPE_BODY_TRACKER_CREATE_INFO_BD};
		info.jointSet = jointSet == BodyJointSet::FullBodyBD ? XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD
															 : XR_BODY_JOINT_SET_BODY_WITHOUT_ARM_BD;
		CheckXr(create(session, &info, &m_bd), "Create physical BD body tracker");
	}
	if (!m_fb && !m_bd)
		throw std::runtime_error("Physical runtime returned a null body tracker");
	m_jointSet = jointSet;
}

void HeadsetBody::Close()
{
	if (m_runtime.ResourcesPreserved())
		return;
	if (m_fb)
		m_destroyFB(m_fb);
	if (m_bd)
		m_destroyBD(m_bd);
	m_fb = XR_NULL_HANDLE;
	m_bd = XR_NULL_HANDLE;
	m_skeleton.reset();
	m_skeletonRevision.reset();
}

static PhysicalBodyJoint ReadJoint(XrPosef pose, XrSpaceLocationFlags flags)
{
	PhysicalBodyJoint result;
	result.flags = FinitePoseFlags(pose, flags) & TrackedPoseFlags;
	if (result.flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)
		result.pose.position = pose.position;
	else
		result.flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	if (result.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)
		result.pose.orientation = pose.orientation;
	else
		result.flags &= ~XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	const auto normalized = NormalizedPose(result.pose);
	if (!normalized)
		throw std::runtime_error("Physical runtime returned an unusable body joint");
	result.pose = *normalized;
	return result;
}

PhysicalBodySample HeadsetBody::Sample(XrTime time, XrSpace baseSpace, bool locate)
{
	if (!m_fb && !m_bd)
		throw std::logic_error("Physical body provider is not open");
	if (time <= 0 || !baseSpace)
		throw std::invalid_argument("Physical body observation requires a positive time and base space");
	PhysicalBodySample result;
	result.jointSet = m_jointSet;
	result.requestedTime = time;
	const auto map = BodyModel::JointMap(m_jointSet);
	if (!locate)
	{
		for (const auto joint : map)
			result.joints[static_cast<std::size_t>(joint)] = PhysicalBodyJoint{};
		return result;
	}
	if (m_fb)
	{
		std::array<XrBodyJointLocationFB, BodyJointCount> joints{};
		XrBodyJointsLocateInfoFB info{XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB};
		info.baseSpace = baseSpace;
		info.time = time;
		XrBodyJointLocationsFB locations{XR_TYPE_BODY_JOINT_LOCATIONS_FB};
		locations.jointCount = static_cast<std::uint32_t>(map.size());
		locations.jointLocations = joints.data();
		CheckXr(m_locateFB(m_fb, &info, &locations), "Locate physical FB body joints");
		if (locations.jointCount != map.size() || locations.jointLocations != joints.data() ||
			locations.isActive > XR_TRUE || !std::isfinite(locations.confidence) || locations.confidence < 0 ||
			locations.confidence > 1 || (locations.isActive && locations.time <= 0))
			throw std::runtime_error("Physical runtime returned invalid body observation metadata");
		result.active = locations.isActive == XR_TRUE;
		result.confidence = locations.confidence;
		result.returnedTime = locations.time;
		result.skeletonRevision = locations.skeletonChangedCount;
		for (std::size_t index = 0; index < map.size(); ++index)
			result.joints[static_cast<std::size_t>(map[index])] =
				ReadJoint(joints[index].pose, locations.isActive ? joints[index].locationFlags : 0);
		if (!m_skeletonRevision || *m_skeletonRevision != locations.skeletonChangedCount)
		{
			m_skeleton = ReadSkeleton();
			m_skeletonRevision = locations.skeletonChangedCount;
		}
		result.skeleton = m_skeleton;
	}
	else
	{
		std::array<XrBodyJointLocationBD, BodyJointCount> joints{};
		XrBodyJointsLocateInfoBD info{XR_TYPE_BODY_JOINTS_LOCATE_INFO_BD};
		info.baseSpace = baseSpace;
		info.time = time;
		XrBodyJointLocationsBD locations{XR_TYPE_BODY_JOINT_LOCATIONS_BD};
		locations.jointLocationCount = static_cast<std::uint32_t>(map.size());
		locations.jointLocations = joints.data();
		CheckXr(m_locateBD(m_bd, &info, &locations), "Locate physical BD body joints");
		if (locations.jointLocationCount != map.size() || locations.jointLocations != joints.data() ||
			locations.allJointPosesTracked > XR_TRUE)
			throw std::runtime_error("Physical runtime returned invalid BD body observation metadata");
		result.allJointPosesTracked = locations.allJointPosesTracked == XR_TRUE;
		for (std::size_t index = 0; index < map.size(); ++index)
			result.joints[static_cast<std::size_t>(map[index])] =
				ReadJoint(joints[index].pose, joints[index].locationFlags);
	}
	result.located = true;
	return result;
}

PhysicalBodySkeleton HeadsetBody::ReadSkeleton() const
{
	const auto map = BodyModel::JointMap(m_jointSet);
	std::array<XrBodySkeletonJointFB, BodyJointCount> joints{};
	XrBodySkeletonFB skeleton{XR_TYPE_BODY_SKELETON_FB};
	skeleton.jointCount = static_cast<std::uint32_t>(map.size());
	skeleton.joints = joints.data();
	CheckXr(m_skeletonFB(m_fb, &skeleton), "Read physical body skeleton");
	if (skeleton.jointCount != map.size() || skeleton.joints != joints.data())
		throw std::runtime_error("Physical runtime changed the body skeleton buffer");
	PhysicalBodySkeleton result;
	for (std::size_t index = 0; index < map.size(); ++index)
	{
		const auto& value = joints[index];
		if (value.joint < 0 || static_cast<std::size_t>(value.joint) >= map.size() || value.parentJoint < -1 ||
			(value.parentJoint >= 0 && static_cast<std::size_t>(value.parentJoint) >= map.size()))
			throw std::runtime_error("Physical body skeleton contains an unknown joint");
		const auto joint = map[value.joint];
		auto& output = result[static_cast<std::size_t>(joint)];
		const auto pose = NormalizedPose(value.pose);
		if (output || !pose)
			throw std::runtime_error("Physical body skeleton contains a duplicate or invalid joint");
		output =
			SkeletonJoint{joint, value.parentJoint == -1 ? std::nullopt : std::optional{map[value.parentJoint]}, *pose};
	}
	for (const auto joint : map)
	{
		auto parent = result[static_cast<std::size_t>(joint)]->parent;
		for (std::size_t depth = 0; parent; ++depth)
		{
			if (depth >= map.size())
				throw std::runtime_error("Physical body skeleton contains a parent cycle");
			parent = result[static_cast<std::size_t>(*parent)]->parent;
		}
	}
	return result;
}
} // namespace simxr::headset
