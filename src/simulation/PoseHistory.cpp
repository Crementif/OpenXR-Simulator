#include "simulation/PoseHistory.h"
#include "simulation/TrackingSystem.h"

#include <algorithm>
#include <cmath>
#include <iterator>
#include <numbers>
#include <stdexcept>
#include <utility>

namespace simxr
{
namespace
{
XrVector3f Interpolate(XrVector3f a, XrVector3f b, float fraction)
{
	return {std::lerp(a.x, b.x, fraction), std::lerp(a.y, b.y, fraction), std::lerp(a.z, b.z, fraction)};
}

XrQuaternionf Interpolate(XrQuaternionf a, XrQuaternionf b, float fraction)
{
	float dot = a.x * b.x + a.y * b.y + a.z * b.z + a.w * b.w;
	if (dot < 0)
	{
		b = {-b.x, -b.y, -b.z, -b.w};
		dot = -dot;
	}
	float left = 1 - fraction;
	float right = fraction;
	if (dot < 0.9995f)
	{
		float angle = std::acos(std::clamp(dot, -1.0f, 1.0f));
		left = std::sin((1 - fraction) * angle) / std::sin(angle);
		right = std::sin(fraction * angle) / std::sin(angle);
	}
	XrQuaternionf result{left * a.x + right * b.x, left * a.y + right * b.y, left * a.z + right * b.z,
						 left * a.w + right * b.w};
	float length = std::sqrt(result.x * result.x + result.y * result.y + result.z * result.z + result.w * result.w);
	return {result.x / length, result.y / length, result.z / length, result.w / length};
}

XrPosef Interpolate(XrPosef a, XrPosef b, float fraction)
{
	return {Interpolate(a.orientation, b.orientation, fraction), Interpolate(a.position, b.position, fraction)};
}

float InterpolateAngle(float a, float b, float fraction)
{
	return a + std::remainder(b - a, 2 * std::numbers::pi_v<float>) * fraction;
}

bool Continuous(const TrackingSnapshot& a, const TrackingSnapshot& b)
{
	if (a.body.skeletonRevision != b.body.skeletonRevision || a.input.focused != b.input.focused ||
		a.head.flags != b.head.flags || a.input.gaze.available != b.input.gaze.available ||
		a.input.gaze.tracked != b.input.gaze.tracked || a.input.body.active != b.input.body.active ||
		a.input.body.preset != b.input.body.preset ||
		a.input.body.rootPose.has_value() != b.input.body.rootPose.has_value())
		return false;
	for (std::size_t index = 0; index < a.controllers.size(); ++index)
		if (a.controllers[index].connected != b.controllers[index].connected ||
			a.controllers[index].pose.flags != b.controllers[index].pose.flags)
			return false;
	for (std::size_t index = 0; index < a.trackers.size(); ++index)
	{
		const auto& first = a.trackers[index].device;
		const auto& second = b.trackers[index].device;
		if (first.identity != second.identity || first.role != second.role || first.connected != second.connected ||
			first.tracked != second.tracked || first.anchor != second.anchor ||
			first.poseOverride.has_value() != second.poseOverride.has_value() ||
			first.jointOffset.has_value() != second.jointOffset.has_value())
			return false;
	}
	for (std::size_t index = 0; index < a.input.body.controls.size(); ++index)
		if (a.input.body.controls[index].tracked != b.input.body.controls[index].tracked ||
			a.input.body.controls[index].offset.has_value() != b.input.body.controls[index].offset.has_value())
			return false;
	for (std::size_t index = 0; index < a.input.body.joints.size(); ++index)
	{
		const auto& first = a.input.body.joints[index];
		const auto& second = b.input.body.joints[index];
		if (first.has_value() != second.has_value() ||
			(first && (first->tracked != second->tracked || first->pose.has_value() != second->pose.has_value())))
			return false;
	}
	return true;
}
} // namespace

PoseHistory::PoseHistory(std::size_t capacity) : m_capacity(capacity)
{
	if (!capacity)
		throw std::invalid_argument("Pose history requires capacity");
}

void PoseHistory::Push(TrackingSnapshot snapshot, bool replaceCurrent)
{
	std::lock_guard lock(m_mutex);
	if (replaceCurrent && !m_samples.empty() && snapshot.time == m_samples.back().time)
	{
		// An explicit native merge refines the same admitted boundary. It must
		// not create two samples or rewrite any earlier historical timestamp.
		m_samples.back() = std::move(snapshot);
		return;
	}
	if (snapshot.time.value.count() <= 0 || (!m_samples.empty() && snapshot.time <= m_samples.back().time))
		throw std::invalid_argument("Tracking samples must be monotonic");
	m_samples.push_back(std::move(snapshot));
	while (m_samples.size() > m_capacity)
		m_samples.pop_front();
}

std::optional<TrackingSnapshot> PoseHistory::Sample(RuntimeTime time) const
{
	std::lock_guard lock(m_mutex);
	if (m_samples.empty() || time < m_samples.front().time)
		return {};
	auto right =
		std::upper_bound(m_samples.begin(), m_samples.end(), time,
						 [](RuntimeTime value, const TrackingSnapshot& sample) { return value < sample.time; });
	auto left = std::prev(right);
	if (right == m_samples.end() || left->time == time || !Continuous(*left, *right))
		return *left;
	const float fraction = static_cast<float>(static_cast<double>((time.value - left->time.value).count()) /
											  static_cast<double>((right->time.value - left->time.value).count()));
	auto input = left->input;
	input.head.pose = Interpolate(input.head.pose, right->input.head.pose, fraction);
	for (std::size_t index = 0; index < input.controllers.size(); ++index)
		input.controllers[index].pose.pose =
			Interpolate(input.controllers[index].pose.pose, right->input.controllers[index].pose.pose, fraction);
	for (std::size_t index = 0; index < input.trackers.size(); ++index)
		if (input.trackers[index].poseOverride)
			input.trackers[index].poseOverride =
				Interpolate(*input.trackers[index].poseOverride, *right->input.trackers[index].poseOverride, fraction);
	for (std::size_t index = 0; index < input.trackers.size(); ++index)
		if (input.trackers[index].jointOffset)
			input.trackers[index].jointOffset =
				Interpolate(*input.trackers[index].jointOffset, *right->input.trackers[index].jointOffset, fraction);
	input.gaze.yaw = InterpolateAngle(input.gaze.yaw, right->input.gaze.yaw, fraction);
	input.gaze.pitch = InterpolateAngle(input.gaze.pitch, right->input.gaze.pitch, fraction);
	if (input.body.rootPose)
		input.body.rootPose = Interpolate(*input.body.rootPose, *right->input.body.rootPose, fraction);
	input.body.confidence += (right->input.body.confidence - input.body.confidence) * fraction;
	for (std::size_t index = 0; index < input.body.controls.size(); ++index)
		if (input.body.controls[index].offset)
			input.body.controls[index].offset =
				Interpolate(*input.body.controls[index].offset, *right->input.body.controls[index].offset, fraction);
	for (std::size_t index = 0; index < input.body.joints.size(); ++index)
		if (input.body.joints[index] && input.body.joints[index]->pose)
			input.body.joints[index]->pose =
				Interpolate(*input.body.joints[index]->pose, *right->input.body.joints[index]->pose, fraction);
	BodyModel body(left->proportions);
	auto result = TrackingSystem::Evaluate(time, input, body);
	result.body.skeletonRevision = left->body.skeletonRevision;
	return result;
}

std::size_t PoseHistory::Size() const
{
	std::lock_guard lock(m_mutex);
	return m_samples.size();
}

void PoseHistory::Clear()
{
	std::lock_guard lock(m_mutex);
	m_samples.clear();
}
} // namespace simxr
