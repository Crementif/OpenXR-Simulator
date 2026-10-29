#include "runtime/extensions/BodyTracking.h"
#include "Pch.h"
#include "math/Math.h"
#include "runtime/validation/StructureChain.h"

namespace simxr
{
static bool Has(XrSpaceLocationFlags flags, XrSpaceLocationFlags bits)
{
	return (flags & bits) == bits;
}

static std::size_t Index(BodyJoint joint)
{
	return static_cast<std::size_t>(joint);
}

static bool SameObservedSkeleton(const std::optional<BodyObservation>& first,
								 const std::optional<BodyObservation>& second)
{
	if (first.has_value() != second.has_value())
		return false;
	if (!first)
		return true;
	if (first->jointSet != second->jointSet || first->skeletonRevision != second->skeletonRevision ||
		first->skeleton.has_value() != second->skeleton.has_value())
		return false;
	if (!first->skeleton)
		return true;
	for (std::size_t index = 0; index < BodyJointCount; ++index)
	{
		const auto& a = (*first->skeleton)[index];
		const auto& b = (*second->skeleton)[index];
		if (a.has_value() != b.has_value())
			return false;
		if (!a)
			continue;
		if (a->joint != b->joint || a->parent != b->parent || a->bindPose.position.x != b->bindPose.position.x ||
			a->bindPose.position.y != b->bindPose.position.y || a->bindPose.position.z != b->bindPose.position.z ||
			a->bindPose.orientation.x != b->bindPose.orientation.x ||
			a->bindPose.orientation.y != b->bindPose.orientation.y ||
			a->bindPose.orientation.z != b->bindPose.orientation.z ||
			a->bindPose.orientation.w != b->bindPose.orientation.w)
			return false;
	}
	return true;
}

BodyTracking::BodyTracking(XrSession owner, const TrackingSystem& tracking, const SpaceSystem& spaces,
						   const EnabledExtensions& enabled, const BodyTrackingCapabilities& capabilities)
	: m_owner(owner), m_tracking(tracking), m_spaces(spaces), m_enabled(enabled), m_capabilities(capabilities)
{
	if (owner == XR_NULL_HANDLE)
		throw std::invalid_argument("Missing body tracking owner");
}

bool BodyTracking::Enabled(std::string_view extension) const
{
	return m_enabled.Contains(extension) &&
		   (extension == XR_BD_BODY_TRACKING_EXTENSION_NAME || m_enabled.Contains(XR_FB_BODY_TRACKING_EXTENSION_NAME));
}

XrResult BodyTracking::CreateFB(const XrBodyTrackerCreateInfoFB* info, XrBodyTrackerFB* output)
{
	if (!Enabled(XR_FB_BODY_TRACKING_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (!info || info->type != XR_TYPE_BODY_TRACKER_CREATE_INFO_FB || !output ||
		XR_FAILED(StructureChain(info->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	if (!m_capabilities.fb)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	BodyJointSet jointSet;
	if (info->bodyJointSet == XR_BODY_JOINT_SET_DEFAULT_FB)
		jointSet = BodyJointSet::UpperBodyFB;
	else if (info->bodyJointSet == XR_BODY_JOINT_SET_FULL_BODY_META &&
			 Enabled(XR_META_BODY_TRACKING_FULL_BODY_EXTENSION_NAME))
	{
		if (!m_capabilities.fullBodyMETA)
			return XR_ERROR_FEATURE_UNSUPPORTED;
		jointSet = BodyJointSet::FullBodyMETA;
	}
	else
		return XR_ERROR_VALIDATION_FAILURE;
	auto tracker = std::make_unique<TrackerFB>();
	tracker->jointSet = jointSet;
	*output = m_fb.Insert(m_owner, std::move(tracker));
	return XR_SUCCESS;
}

XrResult BodyTracking::CreateBD(const XrBodyTrackerCreateInfoBD* info, XrBodyTrackerBD* output)
{
	if (!Enabled(XR_BD_BODY_TRACKING_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	if (!info || info->type != XR_TYPE_BODY_TRACKER_CREATE_INFO_BD || !output ||
		XR_FAILED(StructureChain(info->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	if (!m_capabilities.bd)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	BodyJointSet jointSet;
	if (info->jointSet == XR_BODY_JOINT_SET_FULL_BODY_JOINTS_BD)
		jointSet = BodyJointSet::FullBodyBD;
	else if (info->jointSet == XR_BODY_JOINT_SET_BODY_WITHOUT_ARM_BD)
		jointSet = BodyJointSet::WithoutArmsBD;
	else
		return XR_ERROR_VALIDATION_FAILURE;
	auto tracker = std::make_unique<TrackerBD>();
	tracker->jointSet = jointSet;
	*output = m_bd.Insert(m_owner, std::move(tracker));
	return XR_SUCCESS;
}

XrResult BodyTracking::DestroyFB(XrBodyTrackerFB tracker)
{
	if (!Enabled(XR_FB_BODY_TRACKING_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return m_fb.Erase(tracker) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XrResult BodyTracking::DestroyBD(XrBodyTrackerBD tracker)
{
	if (!Enabled(XR_BD_BODY_TRACKING_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	return m_bd.Erase(tracker) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

XrResult BodyTracking::QueryAt(XrSpace base, XrTime time, std::optional<Query>& query) const
{
	PoseSample basePose;
	auto result = m_spaces.WorldPose(base, {Duration{time}}, basePose);
	if (XR_FAILED(result))
		return result;
	auto snapshot = m_tracking.Sample({Duration{time}});
	if (snapshot)
		query = Query{std::move(*snapshot), basePose, {Duration{time}}};
	else
		query.reset();
	return XR_SUCCESS;
}

TrackingSnapshot BodyTracking::Calibrated(TrackingSnapshot snapshot, const std::optional<float>& height)
{
	if (!height || snapshot.input.bodyObservation)
		return snapshot;
	auto proportions = snapshot.proportions;
	const double scale = static_cast<double>(*height) / proportions.height;
	const double shoulderWidth = proportions.shoulderWidth * scale;
	const double hipWidth = proportions.hipWidth * scale;
	if (shoulderWidth > std::numeric_limits<float>::max() || hipWidth > std::numeric_limits<float>::max())
		throw std::invalid_argument("Calibrated body proportions are not representable");
	proportions.height = *height;
	proportions.shoulderWidth = static_cast<float>(shoulderWidth);
	proportions.hipWidth = static_cast<float>(hipWidth);
	BodyModel model(proportions);
	auto result = TrackingSystem::Evaluate(snapshot.time, snapshot.input, model);
	result.body.skeletonRevision = snapshot.body.skeletonRevision;
	return result;
}

PoseSample BodyTracking::InBase(const PoseSample& joint, const PoseSample& base)
{
	PoseSample result{RelativePose(joint.pose, base.pose), 0, std::min(joint.sampleTime, base.sampleTime)};
	for (auto bit : {XR_SPACE_LOCATION_ORIENTATION_VALID_BIT, XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT})
		if (Has(joint.flags, bit) && Has(base.flags, bit))
			result.flags |= bit;
	if (Has(joint.flags, XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
		Has(base.flags, XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		result.flags |= XR_SPACE_LOCATION_POSITION_VALID_BIT;
	if (Has(joint.flags, XR_SPACE_LOCATION_POSITION_TRACKED_BIT) &&
		Has(base.flags, XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
		result.flags |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	result.flags = FinitePoseFlags(result.pose, result.flags);
	return result;
}

XrResult BodyTracking::LocateFB(XrBodyTrackerFB handle, const XrBodyJointsLocateInfoFB* info,
								XrBodyJointLocationsFB* locations)
{
	if (!Enabled(XR_FB_BODY_TRACKING_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	auto tracker = m_fb.Find(handle, m_owner);
	if (!tracker)
		return XR_ERROR_HANDLE_INVALID;
	auto mapping = BodyModel::JointMap(tracker->jointSet);
	if (!info || info->type != XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB || !locations ||
		locations->type != XR_TYPE_BODY_JOINT_LOCATIONS_FB || locations->jointCount != mapping.size() ||
		!locations->jointLocations || XR_FAILED(StructureChain(info->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	std::array<XrStructureType, 2> known{};
	std::size_t knownCount = 0;
	if (Enabled(XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME))
		known[knownCount++] = XR_TYPE_BODY_TRACKING_CALIBRATION_STATUS_META;
	if (Enabled(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME))
		known[knownCount++] = XR_TYPE_BODY_TRACKING_FIDELITY_STATUS_META;
	StructureChain chain(locations->next);
	if (XR_FAILED(chain.Validate(std::span(known).first(knownCount))))
		return XR_ERROR_VALIDATION_FAILURE;
	std::optional<Query> query;
	auto result = QueryAt(info->baseSpace, info->time, query);
	if (XR_FAILED(result))
		return result;
	std::lock_guard lock(tracker->mutex);
	bool active = false;
	if (query)
	{
		query->snapshot = Calibrated(std::move(query->snapshot), tracker->state.heightOverride);
		const auto& observed = query->snapshot.input.bodyObservation;
		const bool sourceChanged =
			tracker->lastQuery && !SameObservedSkeleton(observed, tracker->lastQuery->snapshot.input.bodyObservation);
		if (!tracker->lastProportions || *tracker->lastProportions != query->snapshot.proportions || sourceChanged)
		{
			if (tracker->state.skeletonRevision == std::numeric_limits<std::uint32_t>::max())
				return XR_ERROR_RUNTIME_FAILURE;
			++tracker->state.skeletonRevision;
			tracker->lastProportions = query->snapshot.proportions;
		}
		active = query->snapshot.body.active && Has(query->base.flags, ValidPoseFlags);
		if (query->snapshot.input.bodyObservation)
			for (const auto joint : mapping)
				active &= Has(InBase(query->snapshot.body.joints[Index(joint)], query->base).flags, ValidPoseFlags);
		tracker->lastQuery = *query;
	}
	locations->isActive = active ? XR_TRUE : XR_FALSE;
	locations->confidence = active ? query->snapshot.body.confidence : 0;
	locations->time = query ? query->snapshot.body.sampleTime.value.count() : 0;
	locations->skeletonChangedCount = tracker->state.skeletonRevision;
	for (std::size_t index = 0; index < mapping.size(); ++index)
	{
		auto pose = query ? InBase(query->snapshot.body.joints[Index(mapping[index])], query->base) : PoseSample{};
		locations->jointLocations[index] = {active ? pose.flags : 0, pose.pose};
	}
	if (Enabled(XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME))
		if (auto status = const_cast<XrBodyTrackingCalibrationStatusMETA*>(
				chain.Find<XrBodyTrackingCalibrationStatusMETA>(XR_TYPE_BODY_TRACKING_CALIBRATION_STATUS_META)))
			status->status =
				query ? XR_BODY_TRACKING_CALIBRATION_STATE_VALID_META : XR_BODY_TRACKING_CALIBRATION_STATE_INVALID_META;
	if (Enabled(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME))
		if (auto status = const_cast<XrBodyTrackingFidelityStatusMETA*>(
				chain.Find<XrBodyTrackingFidelityStatusMETA>(XR_TYPE_BODY_TRACKING_FIDELITY_STATUS_META)))
			status->fidelity = tracker->state.actualFidelity;
	return XR_SUCCESS;
}

XrResult BodyTracking::LocateBD(XrBodyTrackerBD handle, const XrBodyJointsLocateInfoBD* info,
								XrBodyJointLocationsBD* locations)
{
	if (!Enabled(XR_BD_BODY_TRACKING_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	auto tracker = m_bd.Find(handle, m_owner);
	if (!tracker)
		return XR_ERROR_HANDLE_INVALID;
	auto mapping = BodyModel::JointMap(tracker->jointSet);
	if (!info || info->type != XR_TYPE_BODY_JOINTS_LOCATE_INFO_BD || !locations ||
		locations->type != XR_TYPE_BODY_JOINT_LOCATIONS_BD || locations->jointLocationCount != mapping.size() ||
		!locations->jointLocations || XR_FAILED(StructureChain(info->next).Validate({})) ||
		XR_FAILED(StructureChain(locations->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	std::optional<Query> query;
	auto result = QueryAt(info->baseSpace, info->time, query);
	if (XR_FAILED(result))
		return result;
	locations->allJointPosesTracked = XR_TRUE;
	for (std::size_t index = 0; index < mapping.size(); ++index)
	{
		auto pose = query ? InBase(query->snapshot.body.joints[Index(mapping[index])], query->base) : PoseSample{};
		locations->jointLocations[index] = {pose.flags, pose.pose};
		if (!Has(pose.flags, TrackedPoseFlags))
			locations->allJointPosesTracked = XR_FALSE;
	}
	return XR_SUCCESS;
}

XrResult BodyTracking::SkeletonFB(XrBodyTrackerFB handle, XrBodySkeletonFB* skeleton) const
{
	if (!Enabled(XR_FB_BODY_TRACKING_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	auto tracker = m_fb.Find(handle, m_owner);
	if (!tracker)
		return XR_ERROR_HANDLE_INVALID;
	auto mapping = BodyModel::JointMap(tracker->jointSet);
	if (!skeleton || skeleton->type != XR_TYPE_BODY_SKELETON_FB || skeleton->jointCount != mapping.size() ||
		!skeleton->joints || XR_FAILED(StructureChain(skeleton->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	std::lock_guard lock(tracker->mutex);
	auto query = tracker->lastQuery;
	if (!query)
	{
		auto latest = m_tracking.Sample({Duration{std::numeric_limits<Duration::rep>::max()}});
		TrackingSnapshot snapshot =
			latest ? Calibrated(std::move(*latest), tracker->state.heightOverride)
				   : Calibrated(TrackingSystem::Evaluate({Duration{1}}, TrackingState{}, BodyModel{}),
								tracker->state.heightOverride);
		query = Query{std::move(snapshot), {IdentityPose(), TrackedPoseFlags}, {Duration{1}}};
	}
	if (const auto& observed = query->snapshot.input.bodyObservation)
	{
		if (!observed->skeleton)
			return XR_ERROR_RUNTIME_FAILURE;
		std::array<XrBodySkeletonJointFB, BodyJointCount> joints{};
		for (std::size_t index = 0; index < mapping.size(); ++index)
		{
			const auto& joint = (*observed->skeleton)[Index(mapping[index])];
			if (!joint)
				return XR_ERROR_RUNTIME_FAILURE;
			auto parent = joint->parent;
			while (parent && std::find(mapping.begin(), mapping.end(), *parent) == mapping.end())
				parent = (*observed->skeleton)[Index(*parent)]->parent;
			const auto parentIndex =
				parent ? static_cast<std::int32_t>(std::find(mapping.begin(), mapping.end(), *parent) - mapping.begin())
					   : -1;
			joints[index] = {static_cast<std::int32_t>(index), parentIndex, joint->bindPose};
		}
		std::copy_n(joints.begin(), mapping.size(), skeleton->joints);
		return XR_SUCCESS;
	}
	if (!Has(query->base.flags, ValidPoseFlags))
		return XR_ERROR_RUNTIME_FAILURE;
	auto head = query->snapshot.head;
	auto forward = Rotate(head.pose.orientation, {0, 0, -1});
	head.pose.orientation = EulerAngles(std::atan2(-forward.x, -forward.z), 0, 0);
	head.pose.position.y = query->snapshot.proportions.height;
	BodyControls controls;
	controls.preset = BodyPreset::TPose;
	auto body = BodyModel(query->snapshot.proportions).Evaluate(head, controls, query->snapshot.time);
	std::array<XrBodySkeletonJointFB, BodyJointCount> joints{};
	for (std::size_t index = 0; index < mapping.size(); ++index)
	{
		auto joint = mapping[index];
		auto parent = body.skeleton[Index(joint)].parent;
		while (parent && std::find(mapping.begin(), mapping.end(), *parent) == mapping.end())
			parent = body.skeleton[Index(*parent)].parent;
		auto parentIndex =
			parent ? static_cast<std::int32_t>(std::find(mapping.begin(), mapping.end(), *parent) - mapping.begin())
				   : (tracker->jointSet == BodyJointSet::FullBodyMETA
						  ? static_cast<std::int32_t>(XR_FULL_BODY_JOINT_NONE_META)
						  : static_cast<std::int32_t>(XR_BODY_JOINT_NONE_FB));
		const auto pose = RelativePose(body.joints[Index(joint)].pose, query->base.pose);
		if (!NormalizedPose(pose))
			return XR_ERROR_RUNTIME_FAILURE;
		joints[index] = {static_cast<std::int32_t>(index), parentIndex, pose};
	}
	std::copy_n(joints.begin(), mapping.size(), skeleton->joints);
	return XR_SUCCESS;
}

XrResult BodyTracking::SuggestCalibrationFB(XrBodyTrackerFB handle, const XrBodyTrackingCalibrationInfoMETA* info)
{
	if (!Enabled(XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	auto tracker = m_fb.Find(handle, m_owner);
	if (!tracker)
		return XR_ERROR_HANDLE_INVALID;
	if (!info || info->type != XR_TYPE_BODY_TRACKING_CALIBRATION_INFO_META || !std::isfinite(info->bodyHeight) ||
		info->bodyHeight <= 0.5f || info->bodyHeight >= 3 || XR_FAILED(StructureChain(info->next).Validate({})))
		return XR_ERROR_VALIDATION_FAILURE;
	if (!m_capabilities.calibrationMETA)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	std::lock_guard lock(tracker->mutex);
	tracker->state.heightOverride = info->bodyHeight;
	return XR_SUCCESS;
}

XrResult BodyTracking::ResetCalibrationFB(XrBodyTrackerFB handle)
{
	if (!Enabled(XR_META_BODY_TRACKING_CALIBRATION_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	auto tracker = m_fb.Find(handle, m_owner);
	if (!tracker)
		return XR_ERROR_HANDLE_INVALID;
	if (!m_capabilities.calibrationMETA)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	std::lock_guard lock(tracker->mutex);
	tracker->state.heightOverride.reset();
	return XR_SUCCESS;
}

XrResult BodyTracking::RequestFidelityFB(XrBodyTrackerFB handle, XrBodyTrackingFidelityMETA fidelity)
{
	if (!Enabled(XR_META_BODY_TRACKING_FIDELITY_EXTENSION_NAME))
		return XR_ERROR_FUNCTION_UNSUPPORTED;
	auto tracker = m_fb.Find(handle, m_owner);
	if (!tracker)
		return XR_ERROR_HANDLE_INVALID;
	if (fidelity != XR_BODY_TRACKING_FIDELITY_LOW_META && fidelity != XR_BODY_TRACKING_FIDELITY_HIGH_META)
		return XR_ERROR_VALIDATION_FAILURE;
	if (!m_capabilities.fidelityMETA)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	std::lock_guard lock(tracker->mutex);
	tracker->state.requestedFidelity = fidelity;
	return XR_SUCCESS;
}

bool BodyTracking::OwnsFB(XrBodyTrackerFB tracker) const
{
	return static_cast<bool>(m_fb.Find(tracker, m_owner));
}

bool BodyTracking::OwnsBD(XrBodyTrackerBD tracker) const
{
	return static_cast<bool>(m_bd.Find(tracker, m_owner));
}

std::optional<BodyTrackerState> BodyTracking::SnapshotFB(XrBodyTrackerFB handle) const
{
	auto tracker = m_fb.Find(handle, m_owner);
	if (!tracker)
		return std::nullopt;
	std::lock_guard lock(tracker->mutex);
	return tracker->state;
}

BodyTrackingCapabilities BodyTracking::Capabilities() const
{
	auto capabilities = m_capabilities;
	capabilities.fullBodyMETA &= capabilities.fb;
	capabilities.calibrationMETA &= capabilities.fb;
	capabilities.fidelityMETA &= capabilities.fb;
	return capabilities;
}

void BodyTracking::Clear()
{
	auto retiredFB = m_fb.RetireAll();
	auto retiredBD = m_bd.RetireAll();
}
} // namespace simxr
