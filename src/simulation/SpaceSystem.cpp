#include "simulation/SpaceSystem.h"
#include "Pch.h"
#include "math/Math.h"

namespace simxr
{
static bool IsSupportedReferenceSpace(XrReferenceSpaceType type)
{
	return type == XR_REFERENCE_SPACE_TYPE_VIEW || type == XR_REFERENCE_SPACE_TYPE_LOCAL ||
		   type == XR_REFERENCE_SPACE_TYPE_STAGE;
}

static XrPosef FloorOrigin(XrPosef local, XrPosef stage)
{
	const auto forward = Rotate(local.orientation, {0, 0, -1});
	return {EulerAngles(std::atan2(-forward.x, -forward.z), 0, 0),
			{local.position.x, stage.position.y, local.position.z}};
}

static bool HasFlags(XrSpaceLocationFlags flags, XrSpaceLocationFlags bits)
{
	return (flags & bits) == bits;
}

static PoseSample ApplyPoseOffset(PoseSample source, XrPosef offset)
{
	const bool translated = offset.position.x != 0 || offset.position.y != 0 || offset.position.z != 0;
	if (translated && !HasFlags(source.flags, XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		source.flags &= ~(XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_POSITION_TRACKED_BIT);
	if (translated && !HasFlags(source.flags, XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
		source.flags &= ~XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	source.pose = Compose(source.pose, offset);
	source.flags = FinitePoseFlags(source.pose, source.flags);
	return source;
}

SpaceSystem::SpaceSystem(XrSession owner, const TrackingSystem& tracking, ActionSource source,
						 ActionValidator validator, EventQueue* events, bool combinedEye, bool localFloor)
	: m_ownerSession(owner), m_tracking(tracking), m_actionSource(std::move(source)),
	  m_actionValidator(std::move(validator)), m_eventQueue(events), m_combinedEye(combinedEye),
	  m_localFloor(localFloor)
{
	if (owner == XR_NULL_HANDLE)
		throw std::invalid_argument("Missing space owner");
	m_origins.reserve(OriginHistoryCapacity);
	m_origins.push_back({{}, IdentityPose(), IdentityPose()});
}

XrResult SpaceSystem::CreateReferenceSpace(XrReferenceSpaceType type, XrPosef offset, XrSpace& output)
{
	if (!IsSupportedReferenceSpace(type) && !(m_localFloor && type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) &&
		!(m_combinedEye && type == XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO))
		return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
	const auto normalized = NormalizedPose(offset);
	if (!normalized)
		return XR_ERROR_POSE_INVALID;
	auto record = std::make_unique<SpaceRecord>();
	record->referenceType = type;
	record->poseOffset = *normalized;
	output = m_spaces.Insert(m_ownerSession, std::move(record));
	return XR_SUCCESS;
}

XrResult SpaceSystem::CreateActionSpace(XrAction action, XrPath subaction, XrPosef offset, XrSpace& output)
{
	if (action == XR_NULL_HANDLE || !m_actionValidator || !m_actionSource)
		return XR_ERROR_HANDLE_INVALID;
	auto result = m_actionValidator(action, subaction);
	if (XR_FAILED(result))
		return result;
	const auto normalized = NormalizedPose(offset);
	if (!normalized)
		return XR_ERROR_POSE_INVALID;
	auto record = std::make_unique<SpaceRecord>();
	record->action = action;
	record->subactionPath = subaction;
	record->poseOffset = *normalized;
	output = m_spaces.Insert(m_ownerSession, std::move(record));
	return XR_SUCCESS;
}

XrResult SpaceSystem::Destroy(XrSpace space)
{
	return m_spaces.Erase(space) ? XR_SUCCESS : XR_ERROR_HANDLE_INVALID;
}

void SpaceSystem::RemoveAction(XrAction action)
{
	for (auto handle : m_spaces.Children(m_ownerSession))
	{
		auto record = m_spaces.Find(handle, m_ownerSession);
		if (record && record->action == action)
			m_spaces.Erase(handle);
	}
}

void SpaceSystem::Clear()
{
	auto retired = m_spaces.RetireAll();
}

bool SpaceSystem::Owns(XrSpace space) const
{
	return static_cast<bool>(m_spaces.Find(space, m_ownerSession));
}

XrResult SpaceSystem::Sample(XrSpace space, RuntimeTime time, SpaceSample& output) const
{
	auto record = m_spaces.Find(space, m_ownerSession);
	if (!record)
		return XR_ERROR_HANDLE_INVALID;
	if (time.value.count() <= 0)
		return XR_ERROR_TIME_INVALID;
	output = {Resolve(*record.operator->(), time, OriginAt(time)), record->referenceType, record->poseOffset};
	return XR_SUCCESS;
}

XrResult SpaceSystem::WorldPose(XrSpace space, RuntimeTime time, PoseSample& output) const
{
	SpaceSample sample;
	const auto result = Sample(space, time, sample);
	if (XR_SUCCEEDED(result))
		output = sample.worldPose;
	return result;
}

XrResult SpaceSystem::Locate(XrSpace space, XrSpace base, RuntimeTime time, SpaceLocation& output) const
{
	auto targetRecord = m_spaces.Find(space, m_ownerSession);
	auto baseRecord = m_spaces.Find(base, m_ownerSession);
	if (!targetRecord || !baseRecord)
		return XR_ERROR_HANDLE_INVALID;
	if (time.value.count() <= 0)
		return XR_ERROR_TIME_INVALID;
	const auto& target = *targetRecord.operator->();
	const auto& relativeTo = *baseRecord.operator->();
	output = {};
	output.pose = Relative(target, relativeTo, time);
	if (SameEntity(target, relativeTo))
	{
		output.velocityFlags = XR_SPACE_VELOCITY_LINEAR_VALID_BIT | XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
		return XR_SUCCESS;
	}
	constexpr Duration interval{1000000};
	RuntimeTime previousTime{time.value - interval};
	if (previousTime.value.count() <= 0)
		return XR_SUCCESS;
	auto originNow = OriginAt(time);
	auto originBefore = OriginAt(previousTime);
	if (!originNow || !originBefore || originNow->time != originBefore->time)
		return XR_SUCCESS;
	auto previous = Relative(target, relativeTo, previousTime);
	if (previous.sampleTime != previousTime || output.pose.sampleTime != time)
		return XR_SUCCESS;
	const float seconds = std::chrono::duration<float>(interval).count();
	if (HasFlags(previous.flags, XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
		HasFlags(output.pose.flags, XR_SPACE_LOCATION_POSITION_VALID_BIT))
	{
		const auto& previousPosition = previous.pose.position;
		const auto& currentPosition = output.pose.pose.position;
		output.linearVelocity = {(currentPosition.x - previousPosition.x) / seconds,
								 (currentPosition.y - previousPosition.y) / seconds,
								 (currentPosition.z - previousPosition.z) / seconds};
		if (IsFinite(output.linearVelocity))
			output.velocityFlags |= XR_SPACE_VELOCITY_LINEAR_VALID_BIT;
		else
			output.linearVelocity = {};
	}
	if (HasFlags(previous.flags, XR_SPACE_LOCATION_ORIENTATION_VALID_BIT) &&
		HasFlags(output.pose.flags, XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
	{
		const auto& previousRotation = previous.pose.orientation;
		auto delta = Multiply(output.pose.pose.orientation,
							  {-previousRotation.x, -previousRotation.y, -previousRotation.z, previousRotation.w});
		if (delta.w < 0)
			delta = {-delta.x, -delta.y, -delta.z, -delta.w};
		const float axisLength = std::sqrt(delta.x * delta.x + delta.y * delta.y + delta.z * delta.z);
		float radiansPerSecondScale = 2 / seconds;
		if (axisLength > 0.000001f)
			radiansPerSecondScale = 2 * std::atan2(axisLength, delta.w) / (axisLength * seconds);
		output.angularVelocity = {delta.x * radiansPerSecondScale, delta.y * radiansPerSecondScale,
								  delta.z * radiansPerSecondScale};
		output.velocityFlags |= XR_SPACE_VELOCITY_ANGULAR_VALID_BIT;
	}
	return XR_SUCCESS;
}

XrResult SpaceSystem::Bounds(XrReferenceSpaceType type, XrExtent2Df& output) const
{
	if (!IsSupportedReferenceSpace(type) && !(m_localFloor && type == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR) &&
		!(m_combinedEye && type == XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO))
		return XR_ERROR_REFERENCE_SPACE_UNSUPPORTED;
	std::lock_guard lock(m_mutex);
	output = {};
	if (type != XR_REFERENCE_SPACE_TYPE_STAGE || !m_stageBounds)
		return XR_SPACE_BOUNDS_UNAVAILABLE;
	output = *m_stageBounds;
	return XR_SUCCESS;
}

XrResult SpaceSystem::SetStageBounds(std::optional<XrExtent2Df> bounds)
{
	if (bounds &&
		(!std::isfinite(bounds->width) || !std::isfinite(bounds->height) || bounds->width < 0 || bounds->height < 0))
		return XR_ERROR_VALIDATION_FAILURE;
	std::lock_guard lock(m_mutex);
	m_stageBounds = bounds;
	return XR_SUCCESS;
}

XrResult SpaceSystem::Recenter(RuntimeTime time)
{
	if (time.value.count() <= 0)
		return XR_ERROR_TIME_INVALID;
	const auto sample = m_tracking.Sample(time);
	ReferenceSpaceState state;
	if (!sample || !RecenterReferenceSpaces(state, sample->head))
		return XR_ERROR_POSE_INVALID;
	return ApplyOrigin(time, state, true, true);
}

XrResult SpaceSystem::StartGeneration(RuntimeTime epoch, ReferenceSpaceState state)
{
	if (epoch.value.count() <= 0)
		return XR_ERROR_TIME_INVALID;
	if (!NormalizeReferenceSpaces(state))
		return XR_ERROR_POSE_INVALID;
	std::lock_guard lock(m_mutex);
	m_origins.clear();
	m_origins.push_back({epoch, state.local, state.stage});
	m_stageBounds = state.stageBounds;
	return XR_SUCCESS;
}

ReferenceSpaceState SpaceSystem::RecordingState(RuntimeTime time) const
{
	std::lock_guard lock(m_mutex);
	for (auto origin = m_origins.rbegin(); origin != m_origins.rend(); ++origin)
		if (origin->time <= time)
			return {origin->local, origin->stage, m_stageBounds};
	throw std::invalid_argument("Recording origin precedes reference-space history");
}

bool SpaceSystem::CanPublish(RuntimeTime time) const
{
	std::lock_guard lock(m_mutex);
	return time.value.count() > 0 && time >= m_origins.back().time;
}

XrResult SpaceSystem::Restore(RuntimeTime time, ReferenceSpaceState state, bool requested)
{
	if (!NormalizeReferenceSpaces(state))
		return XR_ERROR_POSE_INVALID;
	return ApplyOrigin(time, state, false, requested);
}

std::optional<SpaceSystem::Origin> SpaceSystem::OriginAt(RuntimeTime time) const
{
	std::lock_guard lock(m_mutex);
	for (auto origin = m_origins.rbegin(); origin != m_origins.rend(); ++origin)
		if (origin->time <= time)
			return *origin;
	return std::nullopt;
}

PoseSample SpaceSystem::Resolve(const SpaceRecord& record, RuntimeTime time, const std::optional<Origin>& origin) const
{
	PoseSample result;
	if (record.action != XR_NULL_HANDLE)
		result = m_actionSource(record.action, record.subactionPath, time);
	else if (record.referenceType == XR_REFERENCE_SPACE_TYPE_VIEW ||
			 record.referenceType == XR_REFERENCE_SPACE_TYPE_COMBINED_EYE_VARJO)
	{
		auto sample = m_tracking.Sample(time);
		if (sample)
			result = record.referenceType == XR_REFERENCE_SPACE_TYPE_VIEW ? sample->head : sample->gaze;
		result.eyeGazeSampleTime.reset();
	}
	else if (origin)
		result = {record.referenceType == XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR
					  ? FloorOrigin(origin->local, origin->stage)
				  : record.referenceType == XR_REFERENCE_SPACE_TYPE_LOCAL ? origin->local
																		  : origin->stage,
				  TrackedPoseFlags, time};
	return ApplyPoseOffset(result, record.poseOffset);
}

bool SpaceSystem::SameEntity(const SpaceRecord& left, const SpaceRecord& right)
{
	return left.referenceType == right.referenceType && left.action == right.action &&
		   left.subactionPath == right.subactionPath;
}

PoseSample SpaceSystem::Relative(const SpaceRecord& target, const SpaceRecord& base, RuntimeTime time) const
{
	if (SameEntity(target, base))
	{
		auto result = PoseSample{RelativePose(target.poseOffset, base.poseOffset), TrackedPoseFlags, time};
		result.eyeGazeSampleTime = Resolve(target, time, OriginAt(time)).eyeGazeSampleTime;
		result.flags = FinitePoseFlags(result.pose, result.flags);
		return result;
	}
	auto origin = OriginAt(time);
	auto targetPose = Resolve(target, time, origin);
	auto basePose = Resolve(base, time, origin);
	PoseSample result{RelativePose(targetPose.pose, basePose.pose), 0,
					  std::min(targetPose.sampleTime, basePose.sampleTime)};
	result.eyeGazeSampleTime = targetPose.eyeGazeSampleTime;
	for (auto bit : {XR_SPACE_LOCATION_ORIENTATION_VALID_BIT, XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT})
		if (HasFlags(targetPose.flags, bit) && HasFlags(basePose.flags, bit))
			result.flags |= bit;
	if (HasFlags(targetPose.flags, XR_SPACE_LOCATION_POSITION_VALID_BIT) &&
		HasFlags(basePose.flags, XR_SPACE_LOCATION_POSITION_VALID_BIT | XR_SPACE_LOCATION_ORIENTATION_VALID_BIT))
		result.flags |= XR_SPACE_LOCATION_POSITION_VALID_BIT;
	if (HasFlags(targetPose.flags, XR_SPACE_LOCATION_POSITION_TRACKED_BIT) &&
		HasFlags(basePose.flags, XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT))
		result.flags |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT;
	result.flags = FinitePoseFlags(result.pose, result.flags);
	constexpr auto tracked = XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
	if (result.eyeGazeSampleTime && !HasFlags(result.flags, tracked))
		result.flags &= ~tracked;
	return result;
}

XrResult SpaceSystem::ApplyOrigin(RuntimeTime time, const ReferenceSpaceState& state, bool requireNewTime,
								  bool requested)
{
	std::lock_guard lock(m_mutex);
	if (time.value.count() <= 0 || time < m_origins.back().time || (requireNewTime && time == m_origins.back().time))
		return XR_ERROR_TIME_INVALID;
	const auto previous = m_origins.back();
	if (!requested && SameReferenceSpaces(state, {previous.local, previous.stage, m_stageBounds}))
		return XR_SUCCESS;
	const Origin next{time, state.local, state.stage};
	if (time == previous.time)
		m_origins.back() = next;
	else
	{
		if (m_origins.size() == OriginHistoryCapacity)
			m_origins.erase(m_origins.begin());
		m_origins.push_back(next);
	}
	if (!requireNewTime)
		m_stageBounds = state.stageBounds;
	if (m_eventQueue)
	{
		const auto localDelta = RelativePose(next.local, previous.local);
		const auto stageDelta = RelativePose(next.stage, previous.stage);
		if (m_localFloor)
		{
			const auto delta =
				RelativePose(FloorOrigin(next.local, next.stage), FloorOrigin(previous.local, previous.stage));
			m_eventQueue->Push(ReferenceSpaceEvent{m_ownerSession, XR_REFERENCE_SPACE_TYPE_LOCAL_FLOOR, time,
												   IsFinite(delta.position), delta});
		}
		m_eventQueue->Push(ReferenceSpaceEvent{m_ownerSession, XR_REFERENCE_SPACE_TYPE_LOCAL, time,
											   IsFinite(localDelta.position), localDelta});
		m_eventQueue->Push(ReferenceSpaceEvent{m_ownerSession, XR_REFERENCE_SPACE_TYPE_STAGE, time,
											   IsFinite(stageDelta.position), stageDelta});
	}
	return XR_SUCCESS;
}
} // namespace simxr
