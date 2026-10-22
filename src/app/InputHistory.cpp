#include "app/InputHistory.h"

#include <algorithm>
#include <cmath>
#include <cstring>
#include <stdexcept>
#include <tuple>

namespace simxr::app
{
namespace
{
bool Same(const XrPosef& a, const XrPosef& b)
{
	const auto& p = a.position;
	const auto& q = a.orientation;
	const auto& r = b.position;
	const auto& o = b.orientation;
	return p.x == r.x && p.y == r.y && p.z == r.z && q.x == o.x && q.y == o.y && q.z == o.z && q.w == o.w;
}
// Sample times change every frame and are not input.
bool Same(const PoseSample& a, const PoseSample& b)
{
	return a.flags == b.flags && Same(a.pose, b.pose);
}
bool Same(const ControllerPose& a, const ControllerPose& b)
{
	return a.connected == b.connected && Same(a.pose, b.pose);
}
bool Same(const ControllerControls& a, const ControllerControls& b)
{
	const auto values = [](const ControllerControls& c)
	{
		return std::tie(c.trigger, c.grip, c.gripForce, c.stick.x, c.stick.y, c.trackpad.x, c.trackpad.y,
						c.trackpadForce, c.primary, c.secondary, c.menu, c.back, c.thumbClick, c.trackpadClick,
						c.primaryTouch, c.secondaryTouch, c.triggerTouch, c.thumbTouch, c.trackpadTouch,
						c.thumbrestTouch, c.connected);
	};
	return values(a) == values(b);
}
template <typename T> bool Same(const std::array<T, 2>& a, const std::array<T, 2>& b)
{
	return Same(a[0], b[0]) && Same(a[1], b[1]);
}
} // namespace
float Intensity(const ControllerControls& controls)
{
	float value = std::max({controls.trigger, controls.grip, std::hypot(controls.stick.x, controls.stick.y),
							std::hypot(controls.trackpad.x, controls.trackpad.y)});
	if (controls.primary || controls.secondary || controls.menu || controls.back || controls.thumbClick ||
		controls.trackpadClick)
		value = 1;
	return std::clamp(value, 0.f, 1.f);
}
InputHistory::InputHistory(std::size_t capacity) : m_samples(std::max<std::size_t>(capacity, 1))
{
}
std::uint64_t InputHistory::Append(InputSample sample)
{
	const auto sequence = m_next++;
	m_samples[sequence % m_samples.size()] = std::move(sample);
	return sequence;
}
const InputSample* InputHistory::Find(std::uint64_t sequence) const noexcept
{
	if (!sequence || sequence >= m_next || m_next - sequence > m_samples.size())
		return nullptr;
	return &m_samples[sequence % m_samples.size()];
}
void InputHistory::Clear() noexcept
{
	// Sequence numbers keep rising so stale references never resolve.
	m_next += m_samples.size();
}
automation::InputRecording TakeFromInput(const std::vector<const InputSample*>& samples,
										 const CommandConfiguration& setup)
{
	if (samples.size() < 2)
		throw std::invalid_argument("Select at least two frames with input");
	const auto& origin = *samples.front();
	for (const auto* sample : samples)
		if (sample->identity.session != origin.identity.session || sample->identity.run != origin.identity.run ||
			sample->seed != origin.seed)
			throw std::invalid_argument("The range spans more than one run");
	automation::InputRecording take;
	take.version = 2;
	take.seed = origin.seed;
	take.originFrame = FrameId{origin.identity.frame};
	take.initialScenarios = origin.scenarios;
	take.initialScenarios[static_cast<std::size_t>(FailureScenario::Recenter)] = false;
	auto& initial = take.initial;
	initial.push_back(SetHeadPose{origin.head});
	for (std::uint32_t hand = 0; hand < 2; ++hand)
	{
		initial.push_back(SetControllerPose{hand, origin.poses[hand]});
		initial.push_back(SetControllerControls{hand, origin.controls[hand]});
		initial.push_back(SetControllerProfile{hand, setup.input.preferredProfiles[hand]});
	}
	initial.push_back(SetBodyControls{setup.tracking.body});
	initial.push_back(SetGaze{setup.tracking.gaze});
	for (std::uint32_t index = 0; index < setup.tracking.trackers.size(); ++index)
		initial.push_back(SetTracker{index, setup.tracking.trackers[index]});
	initial.push_back(SetViewSettings{setup.views});
	initial.push_back(SetBodyProportions{setup.proportions});
	initial.push_back(SetReferenceSpaces{setup.referenceSpaces});

	// Frames whose input changed; a long range keeps an even subset so the take
	// stays within the recorder's 1024-transaction limit.
	std::vector<std::size_t> changed;
	for (std::size_t index = 1; index < samples.size(); ++index)
	{
		const auto& a = *samples[index - 1];
		const auto& b = *samples[index];
		if (b.identity.frame <= a.identity.frame)
			throw std::invalid_argument("The range is not in frame order");
		if (!Same(a.head, b.head) || !Same(a.poses, b.poses) || !Same(a.controls, b.controls))
			changed.push_back(index);
	}
	constexpr std::size_t Limit = 1024;
	const auto stride = (changed.size() + Limit - 1) / Limit;
	const InputSample* previous = &origin;
	for (std::size_t position = 0; position < changed.size(); ++position)
	{
		if (stride > 1 && position % stride != stride - 1 && position + 1 != changed.size())
			continue;
		const auto& sample = *samples[changed[position]];
		automation::RecordedTransaction transaction;
		transaction.id = take.transactions.size() + 1;
		transaction.phase = "applied";
		transaction.frame = FrameId{sample.identity.frame};
		if (!Same(previous->head, sample.head))
			transaction.commands.push_back(SetHeadPose{sample.head});
		for (std::uint32_t hand = 0; hand < 2; ++hand)
		{
			if (!Same(previous->poses[hand], sample.poses[hand]))
				transaction.commands.push_back(SetControllerPose{hand, sample.poses[hand]});
			if (!Same(previous->controls[hand], sample.controls[hand]))
				transaction.commands.push_back(SetControllerControls{hand, sample.controls[hand]});
		}
		if (transaction.commands.empty())
			continue;
		take.transactions.push_back(std::move(transaction));
		previous = &sample;
	}
	take.total = take.transactions.size();
	return take;
}
} // namespace simxr::app
