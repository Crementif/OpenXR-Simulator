#pragma once

#include "runtime/EventQueue.h"
#include "runtime/HandleRegistry.h"
#include "simulation/ReferenceSpaceState.h"
#include "simulation/TrackingSystem.h"

#include <cstddef>
#include <functional>
#include <mutex>
#include <optional>
#include <vector>

namespace simxr
{
struct SpaceLocation
{
	PoseSample pose;
	XrSpaceVelocityFlags velocityFlags = 0;
	XrVector3f linearVelocity{};
	XrVector3f angularVelocity{};
};

class SpaceSystem
{
  public:
	using ActionSource = std::function<PoseSample(XrAction, XrPath, RuntimeTime)>;
	using ActionValidator = std::function<XrResult(XrAction, XrPath)>;

	SpaceSystem(XrSession owner, const TrackingSystem& tracking, ActionSource source = {},
				ActionValidator validator = {}, EventQueue* events = nullptr, bool combinedEye = false);
	XrResult CreateReferenceSpace(XrReferenceSpaceType type, XrPosef offset, XrSpace& output);
	XrResult CreateActionSpace(XrAction action, XrPath subaction, XrPosef offset, XrSpace& output);
	XrResult Destroy(XrSpace space);
	void RemoveAction(XrAction action);
	void Clear();
	bool Owns(XrSpace space) const;
	XrResult WorldPose(XrSpace space, RuntimeTime time, PoseSample& output) const;
	XrResult Locate(XrSpace space, XrSpace base, RuntimeTime time, SpaceLocation& output) const;
	XrResult Bounds(XrReferenceSpaceType type, XrExtent2Df& output) const;
	XrResult SetStageBounds(std::optional<XrExtent2Df> bounds);
	XrResult Recenter(RuntimeTime time);
	// Starts a fresh time domain without invalidating handles or emitting changes.
	// Invalid input leaves the previous history and stage bounds unchanged.
	XrResult StartGeneration(RuntimeTime epoch, ReferenceSpaceState state);
	ReferenceSpaceState RecordingState(RuntimeTime time) const;
	bool CanPublish(RuntimeTime time) const;
	XrResult Restore(RuntimeTime time, ReferenceSpaceState state, bool requested = true);

  private:
	struct SpaceRecord
	{
		XrReferenceSpaceType referenceType = XR_REFERENCE_SPACE_TYPE_MAX_ENUM;
		XrAction action = XR_NULL_HANDLE;
		XrPath subactionPath = XR_NULL_PATH;
		XrPosef poseOffset;
	};
	struct Origin
	{
		RuntimeTime time;
		XrPosef local;
		XrPosef stage;
	};
	std::optional<Origin> OriginAt(RuntimeTime time) const;
	XrResult ApplyOrigin(RuntimeTime time, const ReferenceSpaceState& state, bool requireNewTime, bool requested);
	PoseSample Resolve(const SpaceRecord& record, RuntimeTime time, const std::optional<Origin>& origin) const;
	static bool SameEntity(const SpaceRecord& left, const SpaceRecord& right);
	PoseSample Relative(const SpaceRecord& target, const SpaceRecord& base, RuntimeTime time) const;

	static constexpr std::size_t OriginHistoryCapacity = 128;
	const XrSession m_ownerSession;
	const TrackingSystem& m_tracking;
	ActionSource m_actionSource;
	ActionValidator m_actionValidator;
	EventQueue* const m_eventQueue;
	const bool m_combinedEye;
	HandleRegistry<XrSpace, SpaceRecord, XrSession> m_spaces;
	mutable std::mutex m_mutex;
	std::vector<Origin> m_origins;
	std::optional<XrExtent2Df> m_stageBounds = XrExtent2Df{2, 2};
};
} // namespace simxr
