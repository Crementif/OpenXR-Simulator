#include "simulation/TrackingSystem.h"
#include "Pch.h"
#include "Math.h"

#include <cmath>
#include <numbers>
#include <stdexcept>

namespace simxr
{
static PoseSample Normalize(PoseSample sample, RuntimeTime time)
{
	const auto pose = NormalizedPose(sample.pose);
	if (!pose)
		throw std::invalid_argument("Invalid tracking pose");
	if ((sample.flags & ~TrackedPoseFlags) ||
		((sample.flags & XR_SPACE_LOCATION_POSITION_TRACKED_BIT) &&
		 !(sample.flags & XR_SPACE_LOCATION_POSITION_VALID_BIT)) ||
		((sample.flags & XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT) &&
		 !(sample.flags & XR_SPACE_LOCATION_ORIENTATION_VALID_BIT)))
		throw std::invalid_argument("Invalid tracking flags");
	sample.pose = *pose;
	sample.sampleTime = time;
	return sample;
}

TrackingSystem::TrackingSystem(std::size_t historyCapacity) : m_history(historyCapacity) {}

TrackingSnapshot TrackingSystem::Evaluate(RuntimeTime time, const TrackingState& state, const BodyModel& body)
{
	if (time.value.count() <= 0 || !std::isfinite(state.gaze.yaw) || !std::isfinite(state.gaze.pitch))
		throw std::invalid_argument("Invalid tracking time or gaze");
	TrackingSnapshot snapshot;
	snapshot.time = time;
	snapshot.input = state;
	if (state.body.rootPose)
		snapshot.input.body.rootPose = Normalize({*state.body.rootPose, ValidPoseFlags}, time).pose;
	snapshot.input.gaze.yaw = std::remainder(state.gaze.yaw, 2 * std::numbers::pi_v<float>);
	snapshot.input.gaze.pitch = std::remainder(state.gaze.pitch, 2 * std::numbers::pi_v<float>);
	snapshot.proportions = body.Proportions();
	snapshot.head = Normalize(state.head, time);
	snapshot.input.head = snapshot.head;
	for (std::size_t index = 0; index < snapshot.controllers.size(); ++index)
	{
		snapshot.controllers[index] = state.controllers[index];
		auto& controller = snapshot.controllers[index];
		controller.pose = Normalize(controller.pose, time);
		if (!controller.connected || !state.focused)
			controller.pose.flags = 0;
		snapshot.input.controllers[index].pose = Normalize(state.controllers[index].pose, time);
	}
	for (auto& override : snapshot.input.body.joints)
	{
		if (override && override->pose)
		{
			PoseSample value{*override->pose, ValidPoseFlags};
			override->pose = Normalize(value, time).pose;
		}
	}
	snapshot.gaze.pose =
		Compose(snapshot.head.pose, {EulerAngles(snapshot.input.gaze.yaw, snapshot.input.gaze.pitch, 0), {0, 0, 0}});
	snapshot.gaze.sampleTime = time;
	if (state.focused && state.gaze.available)
	{
		snapshot.gaze.flags = snapshot.head.flags & ValidPoseFlags;
		if (state.gaze.tracked && (snapshot.head.flags & TrackedPoseFlags) == TrackedPoseFlags)
			snapshot.gaze.flags |= XR_SPACE_LOCATION_POSITION_TRACKED_BIT | XR_SPACE_LOCATION_ORIENTATION_TRACKED_BIT;
		if (snapshot.gaze.flags & ValidPoseFlags)
			snapshot.gaze.eyeGazeSampleTime = time;
	}
	snapshot.body = body.Evaluate(snapshot.head, snapshot.input.body, time, state.focused);
	auto trackerHead = snapshot.head;
	trackerHead.flags = TrackedPoseFlags;
	auto trackerControls = snapshot.input.body;
	trackerControls.active = true;
	for (auto& control : trackerControls.controls)
		control.tracked = true;
	for (auto& joint : trackerControls.joints)
		if (joint)
			joint->tracked = true;
	auto trackerBody = body.Evaluate(trackerHead, trackerControls, time, state.focused);
	for (std::size_t index = 0; index < snapshot.trackers.size(); ++index)
	{
		const auto& device = state.trackers[index];
		if (!device.identity.value)
			throw std::invalid_argument("Missing tracker identity");
		for (std::size_t previous = 0; previous < index; ++previous)
		{
			const auto& other = state.trackers[previous];
			if (other.identity == device.identity ||
				(other.connected && device.connected && other.role == device.role &&
				 device.role != TrackerRole::Unassigned))
				throw std::invalid_argument("Duplicate tracker identity or connected role");
		}
		auto& tracker = snapshot.trackers[index];
		tracker.device = device;
		if (device.poseOverride && device.jointOffset)
			throw std::invalid_argument("Tracker cannot have both world and joint-relative poses");
		if (device.jointOffset)
			snapshot.input.trackers[index].jointOffset = Normalize({*device.jointOffset, TrackedPoseFlags}, time).pose;
		if (device.poseOverride)
			snapshot.input.trackers[index].poseOverride =
				Normalize({*device.poseOverride, TrackedPoseFlags}, time).pose;
		auto anchor = device.anchor.value_or(BodyModel::TrackerJoint(device.role));
		if (static_cast<std::size_t>(anchor) >= BodyJointCount)
			throw std::invalid_argument("Invalid tracker anchor");
		tracker.pose = trackerBody.joints[static_cast<std::size_t>(anchor)];
		if (device.jointOffset)
			tracker.pose.pose = Compose(tracker.pose.pose, *snapshot.input.trackers[index].jointOffset);
		if (device.poseOverride)
			tracker.pose = Normalize({*device.poseOverride, TrackedPoseFlags}, time);
		if (!device.connected || !state.focused)
			tracker.pose.flags = 0;
		else if (!device.tracked)
			tracker.pose.flags &= ValidPoseFlags;
	}
	return snapshot;
}

TrackingSnapshot TrackingSystem::Publish(RuntimeTime time, const TrackingState& state, bool replaceCurrent)
{
	std::lock_guard lock(m_mutex);
	auto snapshot = Evaluate(time, state, m_body);
	m_history.Push(snapshot, replaceCurrent);
	return snapshot;
}

std::optional<TrackingSnapshot> TrackingSystem::Sample(RuntimeTime time) const
{
	return m_history.Sample(time);
}

void TrackingSystem::SetBodyProportions(const BodyProportions& proportions)
{
	std::lock_guard lock(m_mutex);
	m_body.SetProportions(proportions);
}

void TrackingSystem::Reset()
{
	std::lock_guard lock(m_mutex);
	m_body = BodyModel{};
	m_history.Clear();
}
} // namespace simxr
