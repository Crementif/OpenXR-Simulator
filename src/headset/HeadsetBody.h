#pragma once

#include "simulation/TrackingTypes.h"

namespace simxr::headset
{
class OpenXrRuntime;
struct PhysicalBodyJoint
{
	XrPosef pose{{0, 0, 0, 1}, {0, 0, 0}};
	XrSpaceLocationFlags flags = 0;
};
using PhysicalBodySkeleton = std::array<std::optional<SkeletonJoint>, BodyJointCount>;
// Provider observations are runtime-estimated joints, never independent hardware trackers.
// An absent joint is outside the provider schema; present invalid joints represent tracking loss.
struct PhysicalBodySample
{
	BodyJointSet jointSet = BodyJointSet::UpperBodyFB;
	XrTime requestedTime = 0;
	bool located = false;
	std::optional<XrTime> returnedTime;
	std::optional<bool> active;
	std::optional<bool> allJointPosesTracked;
	std::optional<float> confidence;
	std::optional<std::uint32_t> skeletonRevision;
	std::array<std::optional<PhysicalBodyJoint>, BodyJointCount> joints;
	std::optional<PhysicalBodySkeleton> skeleton;
};
class HeadsetBody
{
  public:
	explicit HeadsetBody(OpenXrRuntime& runtime);
	~HeadsetBody();
	HeadsetBody(const HeadsetBody&) = delete;
	HeadsetBody& operator=(const HeadsetBody&) = delete;
	void Open(XrSession session, BodyJointSet jointSet);
	PhysicalBodySample Sample(XrTime time, XrSpace baseSpace, bool locate = true);
	void Close();

  private:
	PhysicalBodySkeleton ReadSkeleton() const;
	OpenXrRuntime& m_runtime;
	BodyJointSet m_jointSet = BodyJointSet::UpperBodyFB;
	XrBodyTrackerFB m_fb = XR_NULL_HANDLE;
	XrBodyTrackerBD m_bd = XR_NULL_HANDLE;
	PFN_xrDestroyBodyTrackerFB m_destroyFB = nullptr;
	PFN_xrLocateBodyJointsFB m_locateFB = nullptr;
	PFN_xrGetBodySkeletonFB m_skeletonFB = nullptr;
	PFN_xrDestroyBodyTrackerBD m_destroyBD = nullptr;
	PFN_xrLocateBodyJointsBD m_locateBD = nullptr;
	std::optional<std::uint32_t> m_skeletonRevision;
	std::optional<PhysicalBodySkeleton> m_skeleton;
};
} // namespace simxr::headset
