#include "app/InputHistory.h"
#include "control/SimulationControl.h"

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
bool Same(const XrVector3f& a, const XrVector3f& b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z;
}
template <typename T, typename Equal> bool Same(const std::optional<T>& a, const std::optional<T>& b, Equal equal)
{
	return a.has_value() == b.has_value() && (!a || equal(*a, *b));
}
bool SamePose(const XrPosef& a, const XrPosef& b)
{
	return Same(a, b);
}
bool SameOffset(const XrVector3f& a, const XrVector3f& b)
{
	return Same(a, b);
}
bool Same(const BodyControls& a, const BodyControls& b)
{
	if (a.active != b.active || a.confidence != b.confidence || a.preset != b.preset ||
		!Same(a.rootPose, b.rootPose, SamePose))
		return false;
	for (std::size_t index = 0; index < a.controls.size(); ++index)
		if (a.controls[index].tracked != b.controls[index].tracked ||
			!Same(a.controls[index].offset, b.controls[index].offset, SameOffset))
			return false;
	for (std::size_t index = 0; index < a.joints.size(); ++index)
		if (!Same(a.joints[index], b.joints[index],
				  [](const BodyJointOverride& x, const BodyJointOverride& y)
				  {
					  return x.tracked == y.tracked && Same(x.pose, y.pose, SamePose);
				  }))
			return false;
	return true;
}
bool Same(const TrackerDevice& a, const TrackerDevice& b)
{
	return a.identity == b.identity && a.role == b.role && a.connected == b.connected && a.tracked == b.tracked &&
		   a.anchor == b.anchor && Same(a.poseOverride, b.poseOverride, SamePose) &&
		   Same(a.jointOffset, b.jointOffset, SamePose);
}
bool Same(const TrackerDevices& a, const TrackerDevices& b)
{
	return std::equal(a.begin(), a.end(), b.begin(),
					  [](const TrackerDevice& x, const TrackerDevice& y)
					  {
						  return Same(x, y);
					  });
}
bool Same(const GazeInput& a, const GazeInput& b)
{
	return a.yaw == b.yaw && a.pitch == b.pitch && a.available == b.available && a.tracked == b.tracked;
}
bool Same(const std::vector<std::byte>& a, const std::vector<std::byte>& b)
{
	return a == b;
}
std::vector<SimulationCommand> ContextCommands(const InputSample& sample)
{
	if (!sample.context)
		throw std::invalid_argument("This run did not record its view/profile context");
	const auto fields = control::DecodeFields(*sample.context);
	if (fields.error != control::DecodeError::None || fields.fields.size() != 5)
		throw std::invalid_argument("Invalid recorded input context");
	std::vector<SimulationCommand> commands;
	for (const auto& field : fields.fields)
	{
		auto command = control::DecodeSimulationCommand(field);
		if (!std::holds_alternative<SetViewSettings>(command) &&
			!std::holds_alternative<SetBodyProportions>(command) &&
			!std::holds_alternative<SetControllerProfile>(command) &&
			!std::holds_alternative<SetReferenceSpaces>(command))
			throw std::invalid_argument("Unexpected recorded context command");
		commands.push_back(std::move(command));
	}
	return commands;
}
// Keeps one block per run of equal values; 32k frames of a still body cost one copy.
template <typename T> void Share(std::shared_ptr<const T>& value, std::shared_ptr<const T>& previous)
{
	if (!value)
		return;
	if (previous && previous != value && Same(*previous, *value))
		value = previous;
	previous = value;
}
} // namespace
BodyControls BodyOf(const InputSample& sample)
{
	auto body = *sample.body;
	body.rootPose = sample.bodyRoot;
	return body;
}
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
	Share(sample.body, m_body);
	Share(sample.trackers, m_trackers);
	Share(sample.context, m_context);
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
automation::InputRecording TakeFromInput(const std::vector<const InputSample*>& samples)
{
	if (samples.size() < 2)
		throw std::invalid_argument("Select at least two frames with input");
	const auto& origin = *samples.front();
	for (const auto* sample : samples)
	{
		if (sample->identity.session != origin.identity.session || sample->identity.run != origin.identity.run ||
			sample->seed != origin.seed)
			throw std::invalid_argument("The range spans more than one run");
		if (!sample->context)
			throw std::invalid_argument("This run did not record its view/profile context");
		if (!sample->body || !sample->trackers || !sample->gaze)
			throw std::invalid_argument("This run did not report body, gaze and tracker input; a take needs all of it");
	}
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
	}
	initial.push_back(SetBodyControls{BodyOf(origin)});
	initial.push_back(SetGaze{*origin.gaze});
	for (std::uint32_t index = 0; index < origin.trackers->size(); ++index)
		initial.push_back(SetTracker{index, (*origin.trackers)[index]});
	for (auto& command : ContextCommands(origin))
		initial.push_back(std::move(command));

	// Frames whose input changed; a long range keeps an even subset so the take
	// stays within the recorder's 1024-transaction limit.
	std::vector<std::size_t> changed;
	for (std::size_t index = 1; index < samples.size(); ++index)
	{
		const auto& a = *samples[index - 1];
		const auto& b = *samples[index];
		if (b.identity.frame <= a.identity.frame)
			throw std::invalid_argument("The range is not in frame order");
		if (!Same(a.head, b.head) || !Same(a.poses, b.poses) || !Same(a.controls, b.controls) ||
			!Same(*a.gaze, *b.gaze) || !Same(a.bodyRoot, b.bodyRoot, SamePose) ||
			(a.body != b.body && !Same(*a.body, *b.body)) ||
			(a.trackers != b.trackers && !Same(*a.trackers, *b.trackers)) ||
			(a.context != b.context && *a.context != *b.context))
			changed.push_back(index);
	}
	// One transaction is kept for the end of the range.
	constexpr std::size_t Limit = 1023;
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
		if (!Same(*previous->gaze, *sample.gaze))
			transaction.commands.push_back(SetGaze{*sample.gaze});
		if (!Same(previous->bodyRoot, sample.bodyRoot, SamePose) ||
			(previous->body != sample.body && !Same(*previous->body, *sample.body)))
			transaction.commands.push_back(SetBodyControls{BodyOf(sample)});
		for (std::uint32_t slot = 0; slot < sample.trackers->size(); ++slot)
			if (!Same((*previous->trackers)[slot], (*sample.trackers)[slot]))
				transaction.commands.push_back(SetTracker{slot, (*sample.trackers)[slot]});
		if (previous->context != sample.context && *previous->context != *sample.context)
			for (auto& command : ContextCommands(sample))
				transaction.commands.push_back(std::move(command));
		if (transaction.commands.empty())
			continue;
		take.transactions.push_back(std::move(transaction));
		previous = &sample;
	}
	// The take lasts to the range's end, so a still tail is replayed too
	// instead of releasing the devices early.
	const auto& last = *samples.back();
	if (previous != &last)
	{
		automation::RecordedTransaction transaction;
		transaction.id = take.transactions.size() + 1;
		transaction.phase = "applied";
		transaction.frame = FrameId{last.identity.frame};
		transaction.commands.push_back(SetHeadPose{last.head});
		take.transactions.push_back(std::move(transaction));
	}
	take.total = take.transactions.size();
	return take;
}
} // namespace simxr::app
