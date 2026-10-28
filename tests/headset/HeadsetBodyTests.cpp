#include "../TestSupport.h"
#include "Pch.h"
#include "headset/HeadsetBody.h"
#include "headset/OpenXrRuntime.h"
#include "simulation/BodyModel.h"

#include <iostream>

namespace simxr::headset::test
{
static HeadsetBodyCapabilities s_capabilities{true, true, true};
static BodyJointSet s_set;
static unsigned s_destroyed = 0, s_skeletonReads = 0;
static std::uint32_t s_revision = 0;
static bool s_active = true, s_preserved = false;
static int s_badSkeleton = 0, s_badMetadata = 0;
static XrResult s_locateResult = XR_SUCCESS;
static XrResult XRAPI_CALL CreateFB(XrSession session, const XrBodyTrackerCreateInfoFB* info, XrBodyTrackerFB* handle)
{
	Require(session != XR_NULL_HANDLE, "Body session");
	s_set =
		info->bodyJointSet == XR_BODY_JOINT_SET_FULL_BODY_META ? BodyJointSet::FullBodyMETA : BodyJointSet::UpperBodyFB;
	*handle = reinterpret_cast<XrBodyTrackerFB>(2);
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL CreateBD(XrSession session, const XrBodyTrackerCreateInfoBD* info, XrBodyTrackerBD* handle)
{
	Require(session != XR_NULL_HANDLE, "Body session");
	s_set = info->jointSet == XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD ? BodyJointSet::FullBodyBD
																	: BodyJointSet::WithoutArmsBD;
	*handle = reinterpret_cast<XrBodyTrackerBD>(3);
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL DestroyFB(XrBodyTrackerFB)
{
	++s_destroyed;
	return XR_SUCCESS;
}
static XrResult XRAPI_CALL DestroyBD(XrBodyTrackerBD)
{
	++s_destroyed;
	return XR_SUCCESS;
}
static XrPosef Pose(std::size_t index)
{
	return {{0, 0, 0, 1}, {static_cast<float>(index), 1, 2}};
}
static XrResult XRAPI_CALL LocateFB(XrBodyTrackerFB, const XrBodyJointsLocateInfoFB* info,
									XrBodyJointLocationsFB* output)
{
	Require(info->time == 100 && info->baseSpace != XR_NULL_HANDLE, "FB prediction/base-space contract");
	Require(output->jointCount == BodyModel::JointMap(s_set).size(), "FB array size");
	output->isActive = s_active;
	output->time = 90;
	output->confidence = 0.75f;
	output->skeletonChangedCount = s_revision;
	for (std::size_t index = 0; index < output->jointCount; ++index)
		output->jointLocations[index] = {ValidPoseFlags, Pose(index)};
	output->jointLocations[0].locationFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	output->jointLocations[0].pose.position.x = std::numeric_limits<float>::quiet_NaN();
	if (s_badMetadata == 1)
		++output->jointCount;
	if (s_badMetadata == 2)
		output->confidence = std::numeric_limits<float>::quiet_NaN();
	if (s_badMetadata == 3)
		output->time = 0;
	return s_locateResult;
}
static XrResult XRAPI_CALL LocateBD(XrBodyTrackerBD, const XrBodyJointsLocateInfoBD* info,
									XrBodyJointLocationsBD* output)
{
	Require(info->time == 100 && info->baseSpace != XR_NULL_HANDLE, "BD prediction/base-space contract");
	Require(output->jointLocationCount == BodyModel::JointMap(s_set).size(), "BD array size");
	output->allJointPosesTracked = false;
	for (std::size_t index = 0; index < output->jointLocationCount; ++index)
		output->jointLocations[index] = {index ? TrackedPoseFlags : 0, Pose(index)};
	if (s_badMetadata == 1)
		++output->jointLocationCount;
	return s_locateResult;
}
static XrResult XRAPI_CALL Skeleton(XrBodyTrackerFB, XrBodySkeletonFB* output)
{
	++s_skeletonReads;
	Require(output->jointCount == BodyModel::JointMap(s_set).size(), "Skeleton array size");
	for (std::size_t index = 0; index < output->jointCount; ++index)
	{
		const auto joint = static_cast<std::int32_t>(output->jointCount - index - 1);
		output->joints[index] = {joint, joint ? 0 : -1, Pose(joint)};
	}
	if (s_badSkeleton == 1)
		output->joints[0].joint = -1;
	if (s_badSkeleton == 2)
		output->joints[0].joint = output->joints[1].joint;
	if (s_badSkeleton == 3)
		output->joints[0].parentJoint = output->joints[0].joint;
	if (s_badSkeleton == 4)
		output->joints[0].parentJoint = output->jointCount;
	if (s_badSkeleton == 5)
		output->joints[0].pose.orientation = {};
	return XR_SUCCESS;
}
template <class Function> static void Reject(Function function)
{
	bool rejected = false;
	try
	{
		function();
	}
	catch (const std::exception&)
	{
		rejected = true;
	}
	Require(rejected, "Invalid provider operation must reject");
}
} // namespace simxr::headset::test
using namespace simxr::headset::test;
namespace simxr::headset
{
OpenXrRuntime::~OpenXrRuntime() = default;
bool OpenXrRuntime::ResourcesPreserved() const
{
	return s_preserved;
}
const HeadsetBodyCapabilities& OpenXrRuntime::BodyCapabilities() const
{
	return s_capabilities;
}
PFN_xrVoidFunction OpenXrRuntime::Procedure(const char* name) const
{
	const std::string_view value{name};
	if (value == "xrCreateBodyTrackerFB")
		return reinterpret_cast<PFN_xrVoidFunction>(CreateFB);
	if (value == "xrDestroyBodyTrackerFB")
		return reinterpret_cast<PFN_xrVoidFunction>(DestroyFB);
	if (value == "xrLocateBodyJointsFB")
		return reinterpret_cast<PFN_xrVoidFunction>(LocateFB);
	if (value == "xrGetBodySkeletonFB")
		return reinterpret_cast<PFN_xrVoidFunction>(Skeleton);
	if (value == "xrCreateBodyTrackerBD")
		return reinterpret_cast<PFN_xrVoidFunction>(CreateBD);
	if (value == "xrDestroyBodyTrackerBD")
		return reinterpret_cast<PFN_xrVoidFunction>(DestroyBD);
	if (value == "xrLocateBodyJointsBD")
		return reinterpret_cast<PFN_xrVoidFunction>(LocateBD);
	throw std::runtime_error("Unexpected body procedure");
}
void CheckXr(XrResult result, const char* operation)
{
	if (XR_FAILED(result))
		throw std::runtime_error(operation);
}
} // namespace simxr::headset
int main()
{
	try
	{
		using namespace simxr;
		using namespace simxr::headset;
		OpenXrRuntime runtime;
		const auto session = reinterpret_cast<XrSession>(1);
		const auto space = reinterpret_cast<XrSpace>(4);
		for (const auto set : {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA, BodyJointSet::WithoutArmsBD,
							   BodyJointSet::FullBodyBD})
		{
			HeadsetBody body(runtime);
			Reject(
				[&]
				{
					body.Sample(100, space);
				});
			body.Open(session, set);
			Require(s_set == set, "Requested native joint set survives creation");
			Reject(
				[&]
				{
					body.Open(session, set);
				});
			s_locateResult = XR_ERROR_RUNTIME_FAILURE;
			const auto suppressed = body.Sample(100, space, false);
			Require(!suppressed.located && !suppressed.returnedTime && !suppressed.active && !suppressed.confidence &&
						!suppressed.skeleton && !suppressed.skeletonRevision && !suppressed.allJointPosesTracked,
					"Focus suppression must not call native locate or fabricate provider metadata");
			for (const auto& joint : suppressed.joints)
				Require(!joint || !joint->flags, "Suppressed body poses stay invalid");
			s_locateResult = XR_SUCCESS;
			const auto sample = body.Sample(100, space);
			Require(sample.located, "Successful observation identifies a native locate");
			const auto map = BodyModel::JointMap(set);
			Require(sample.jointSet == set && sample.requestedTime == 100, "Provider/time identity");
			Require(std::count_if(sample.joints.begin(), sample.joints.end(),
								  [](const auto& joint)
								  {
									  return joint.has_value();
								  }) == static_cast<std::ptrdiff_t>(map.size()),
					"No fabricated joints outside native schema");
			for (std::size_t index = 1; index < map.size(); ++index)
				Require(sample.joints[static_cast<std::size_t>(map[index])]->pose.position.x == index,
						"Semantic joint mapping");
			const bool fb = set == BodyJointSet::UpperBodyFB || set == BodyJointSet::FullBodyMETA;
			if (fb)
			{
				Require(sample.returnedTime == 90 && sample.active == true && sample.confidence == 0.75f &&
							sample.skeletonRevision == s_revision && sample.skeleton && !sample.allJointPosesTracked,
						"FB metadata retained");
				Require(sample.joints[static_cast<std::size_t>(map[0])]->flags ==
								XR_SPACE_LOCATION_ORIENTATION_VALID_BIT &&
							std::isfinite(sample.joints[static_cast<std::size_t>(map[0])]->pose.position.x),
						"Invalid components sanitized without inventing validity");
				for (std::size_t index = 0; index < map.size(); ++index)
				{
					const auto& joint = (*sample.skeleton)[static_cast<std::size_t>(map[index])];
					Require(joint && joint->bindPose.position.x == index &&
								joint->parent == (index ? std::optional{map[0]} : std::nullopt),
							"Unordered skeleton mapped by joint and parent IDs");
				}
				const auto reads = s_skeletonReads;
				body.Sample(100, space);
				Require(s_skeletonReads == reads, "Unchanged skeleton is cached");
				++s_revision;
				body.Sample(100, space);
				Require(s_skeletonReads == reads + 1, "Revised skeleton is refreshed");
				s_active = false;
				const auto lost = body.Sample(100, space);
				for (const auto& joint : lost.joints)
					Require(!joint || !joint->flags, "Inactive FB never retains valid joints");
				s_active = true;
				for (s_badSkeleton = 1; s_badSkeleton <= 5; ++s_badSkeleton)
				{
					++s_revision;
					Reject(
						[&]
						{
							body.Sample(100, space);
						});
				}
				s_badSkeleton = 0;
				Require(body.Sample(100, space).skeletonRevision == s_revision,
						"Failed skeleton refresh remains retryable");
			}
			else
				Require(sample.allJointPosesTracked == false && !sample.active && !sample.confidence &&
							!sample.returnedTime && !sample.skeletonRevision && !sample.skeleton &&
							sample.joints[static_cast<std::size_t>(map[1])]->flags == TrackedPoseFlags,
						"BD partial tracking retained without fabricated FB metadata");
			for (s_badMetadata = 1; s_badMetadata <= (fb ? 3 : 1); ++s_badMetadata)
				Reject(
					[&]
					{
						body.Sample(100, space);
					});
			s_badMetadata = 0;
			s_locateResult = XR_ERROR_RUNTIME_FAILURE;
			Reject(
				[&]
				{
					body.Sample(100, space);
				});
			s_locateResult = XR_SUCCESS;
			const auto destroyed = s_destroyed;
			body.Close();
			body.Close();
			Require(s_destroyed == destroyed + 1, "Body handle destroyed exactly once");
		}
		const auto beforeScope = s_destroyed;
		{
			HeadsetBody scoped(runtime);
			scoped.Open(session, BodyJointSet::FullBodyBD);
		}
		Require(s_destroyed == beforeScope + 1, "RAII releases the session-owned body handle");
		s_capabilities = {};
		HeadsetBody body(runtime);
		Reject(
			[&]
			{
				body.Open(session, BodyJointSet::UpperBodyFB);
			});
		Reject(
			[&]
			{
				body.Open(session, BodyJointSet::FullBodyBD);
			});
		s_capabilities.fb = true;
		Reject(
			[&]
			{
				body.Open(session, BodyJointSet::FullBodyMETA);
			});
		body.Open(session, BodyJointSet::UpperBodyFB);
		s_preserved = true;
		const auto destroyed = s_destroyed;
		body.Close();
		Require(s_destroyed == destroyed, "Uncertain native lifetime preserves body resources");
		Reject(
			[&]
			{
				body.Open(session, BodyJointSet::UpperBodyFB);
			});
		s_preserved = false;
		body.Close();
		std::cout << "Headset body providers passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
