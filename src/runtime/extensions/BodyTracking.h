#pragma once

#include "runtime/ExtensionRegistry.h"
#include "runtime/HandleRegistry.h"
#include "simulation/SpaceSystem.h"

#include <cstdint>
#include <mutex>
#include <optional>
#include <string_view>

namespace simxr
{
struct BodyTrackingCapabilities
{
	bool fb = true;
	bool fullBodyMETA = true;
	bool bd = true;
	bool calibrationMETA = true;
	bool fidelityMETA = true;
};

struct BodyTrackerState
{
	std::optional<float> heightOverride;
	XrBodyTrackingFidelityMETA requestedFidelity = XR_BODY_TRACKING_FIDELITY_HIGH_META;
	XrBodyTrackingFidelityMETA actualFidelity = XR_BODY_TRACKING_FIDELITY_HIGH_META;
	std::uint32_t skeletonRevision = 0;
};

class BodyTracking
{
  public:
	BodyTracking(XrSession owner, const TrackingSystem& tracking, const SpaceSystem& spaces,
				 const EnabledExtensions& enabled, const BodyTrackingCapabilities& capabilities = {});
	XrResult CreateFB(const XrBodyTrackerCreateInfoFB* info, XrBodyTrackerFB* output);
	XrResult CreateBD(const XrBodyTrackerCreateInfoBD* info, XrBodyTrackerBD* output);
	XrResult DestroyFB(XrBodyTrackerFB tracker);
	XrResult DestroyBD(XrBodyTrackerBD tracker);
	XrResult LocateFB(XrBodyTrackerFB tracker, const XrBodyJointsLocateInfoFB* info, XrBodyJointLocationsFB* locations);
	XrResult LocateBD(XrBodyTrackerBD tracker, const XrBodyJointsLocateInfoBD* info, XrBodyJointLocationsBD* locations);
	XrResult SkeletonFB(XrBodyTrackerFB tracker, XrBodySkeletonFB* skeleton) const;
	XrResult SuggestCalibrationFB(XrBodyTrackerFB tracker, const XrBodyTrackingCalibrationInfoMETA* info);
	XrResult ResetCalibrationFB(XrBodyTrackerFB tracker);
	XrResult RequestFidelityFB(XrBodyTrackerFB tracker, XrBodyTrackingFidelityMETA fidelity);
	bool OwnsFB(XrBodyTrackerFB tracker) const;
	bool OwnsBD(XrBodyTrackerBD tracker) const;
	std::optional<BodyTrackerState> SnapshotFB(XrBodyTrackerFB tracker) const;
	BodyTrackingCapabilities Capabilities() const;
	void Clear();

  private:
	struct Query
	{
		TrackingSnapshot snapshot;
		PoseSample base;
		RuntimeTime requestedTime;
	};
	struct TrackerFB
	{
		BodyJointSet jointSet = BodyJointSet::UpperBodyFB;
		mutable std::mutex mutex;
		BodyTrackerState state;
		std::optional<BodyProportions> lastProportions;
		std::optional<Query> lastQuery;
	};
	struct TrackerBD
	{
		BodyJointSet jointSet = BodyJointSet::FullBodyBD;
	};
	bool Enabled(std::string_view extension) const;
	XrResult QueryAt(XrSpace base, XrTime time, std::optional<Query>& query) const;
	static TrackingSnapshot Calibrated(TrackingSnapshot snapshot, const std::optional<float>& height);
	static PoseSample InBase(const PoseSample& joint, const PoseSample& base);

	XrSession m_owner;
	const TrackingSystem& m_tracking;
	const SpaceSystem& m_spaces;
	const EnabledExtensions m_enabled;
	const BodyTrackingCapabilities m_capabilities;
	HandleRegistry<XrBodyTrackerFB, TrackerFB, XrSession> m_fb;
	HandleRegistry<XrBodyTrackerBD, TrackerBD, XrSession> m_bd;
};
} // namespace simxr
