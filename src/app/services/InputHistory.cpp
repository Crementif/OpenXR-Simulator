#include "app/services/InputHistory.h"
#include "Pch.h"
#include "control/SimulationControl.h"
#include "simulation/BodyObservation.h"

#include <tuple>

namespace simxr::app
{
static bool Same(const XrPosef& a, const XrPosef& b)
{
	const auto& p = a.position;
	const auto& q = a.orientation;
	const auto& r = b.position;
	const auto& o = b.orientation;
	return p.x == r.x && p.y == r.y && p.z == r.z && q.x == o.x && q.y == o.y && q.z == o.z && q.w == o.w;
}

// Sample times change every frame and are not input.
static bool Same(const PoseSample& a, const PoseSample& b)
{
	return a.flags == b.flags && Same(a.pose, b.pose);
}

static bool Same(const ControllerPose& a, const ControllerPose& b)
{
	return a.connected == b.connected && Same(a.pose, b.pose) && a.aim.has_value() == b.aim.has_value() &&
		   (!a.aim || Same(*a.aim, *b.aim));
}

static bool Same(const ControllerControls& a, const ControllerControls& b)
{
	const auto values = [](const ControllerControls& c) {
		return std::tie(c.trigger, c.grip, c.gripForce, c.stick.x, c.stick.y, c.trackpad.x, c.trackpad.y,
						c.trackpadForce, c.primary, c.secondary, c.menu, c.back, c.thumbClick, c.trackpadClick,
						c.primaryTouch, c.secondaryTouch, c.triggerTouch, c.thumbTouch, c.trackpadTouch,
						c.thumbrestTouch, c.connected, c.triggerClick, c.physicalProfile);
	};
	return values(a) == values(b);
}

template<typename T>
static bool Same(const std::array<T, 2>& a, const std::array<T, 2>& b)
{
	return Same(a[0], b[0]) && Same(a[1], b[1]);
}

static bool Same(const XrVector3f& a, const XrVector3f& b)
{
	return a.x == b.x && a.y == b.y && a.z == b.z;
}

template<typename T, typename Equal>
static bool Same(const std::optional<T>& a, const std::optional<T>& b, Equal equal)
{
	return a.has_value() == b.has_value() && (!a || equal(*a, *b));
}

static bool SamePose(const XrPosef& a, const XrPosef& b)
{
	return Same(a, b);
}

static bool SameOffset(const XrVector3f& a, const XrVector3f& b)
{
	return Same(a, b);
}

static bool Same(const BodyControls& a, const BodyControls& b)
{
	if (a.active != b.active || a.confidence != b.confidence || a.preset != b.preset ||
		!Same(a.rootPose, b.rootPose, SamePose))
		return false;
	for (std::size_t index = 0; index < a.controls.size(); ++index)
		if (a.controls[index].tracked != b.controls[index].tracked ||
			!Same(a.controls[index].offset, b.controls[index].offset, SameOffset))
			return false;
	for (std::size_t index = 0; index < a.joints.size(); ++index)
		if (!Same(a.joints[index], b.joints[index], [](const BodyJointOverride& x, const BodyJointOverride& y) {
				return x.tracked == y.tracked && Same(x.pose, y.pose, SamePose);
			}))
			return false;
	return true;
}

static bool Same(const BodyObservation& a, const BodyObservation& b)
{
	if (a.jointSet != b.jointSet || a.requestedTime != b.requestedTime || a.returnedTime != b.returnedTime ||
		a.located != b.located || a.active != b.active || a.allJointPosesTracked != b.allJointPosesTracked ||
		a.confidence != b.confidence || a.skeletonRevision != b.skeletonRevision ||
		a.skeleton.has_value() != b.skeleton.has_value())
		return false;
	for (std::size_t index = 0; index < BodyJointCount; ++index)
	{
		if (!Same(a.joints[index], b.joints[index], [](const ObservedBodyJoint& x, const ObservedBodyJoint& y) {
				return x.flags == y.flags && Same(x.pose, y.pose);
			}))
			return false;
		if (a.skeleton &&
			!Same((*a.skeleton)[index], (*b.skeleton)[index], [](const SkeletonJoint& x, const SkeletonJoint& y) {
				return x.joint == y.joint && x.parent == y.parent && Same(x.bindPose, y.bindPose);
			}))
			return false;
	}
	return true;
}

static bool SameObservation(const InputSample& a, const InputSample& b)
{
	return a.bodyObservation == b.bodyObservation ||
		   (a.bodyObservation && b.bodyObservation && Same(*a.bodyObservation, *b.bodyObservation));
}

static SetBodyControls BodyCommand(const InputSample& sample)
{
	SetBodyControls result{BodyOf(sample)};
	if (sample.bodyObservation)
		result.observation = NormalizeBodyObservation(*sample.bodyObservation);
	return result;
}

static bool Same(const TrackerDevice& a, const TrackerDevice& b)
{
	return a.identity == b.identity && a.sourcePath == b.sourcePath && a.poseFlags == b.poseFlags && a.role == b.role &&
		   a.connected == b.connected && a.tracked == b.tracked && a.anchor == b.anchor &&
		   Same(a.poseOverride, b.poseOverride, SamePose) && Same(a.jointOffset, b.jointOffset, SamePose);
}

static bool Same(const TrackerDevices& a, const TrackerDevices& b)
{
	return std::equal(a.begin(), a.end(), b.begin(), b.end(),
					  [](const TrackerDevice& x, const TrackerDevice& y) { return Same(x, y); });
}

static bool Same(const GazeInput& a, const GazeInput& b)
{
	return a.yaw == b.yaw && a.pitch == b.pitch && a.available == b.available && a.tracked == b.tracked;
}

static bool Same(const std::vector<std::byte>& a, const std::vector<std::byte>& b)
{
	return a == b;
}

static std::vector<SimulationCommand> ContextCommands(const InputSample& sample)
{
	if (!sample.context)
		throw std::invalid_argument("This run did not record its view/profile context");
	const auto fields = control::DecodeFields(*sample.context);
	if (fields.error != control::DecodeError::None || (fields.fields.size() != 5 && fields.fields.size() != 6))
		throw std::invalid_argument("Invalid recorded input context");
	std::vector<SimulationCommand> commands;
	std::uint32_t seen = 0;
	for (const auto& field : fields.fields)
	{
		auto command = control::DecodeSimulationCommand(field);
		std::uint32_t bit = 0;
		if (std::holds_alternative<SetViewSettings>(command))
			bit = 1;
		else if (std::holds_alternative<SetBodyProportions>(command))
			bit = 2;
		else if (const auto* profile = std::get_if<SetControllerProfile>(&command); profile && profile->hand < 2)
			bit = 4u << profile->hand;
		else if (std::holds_alternative<SetReferenceSpaces>(command))
			bit = 16;
		else if (std::holds_alternative<SetEyeViews>(command))
			bit = 32;
		if (!bit || (seen & bit))
			throw std::invalid_argument("Missing or duplicate recorded context command");
		seen |= bit;
		commands.push_back(std::move(command));
	}
	if ((seen & 31) != 31)
		throw std::invalid_argument("Missing recorded view/profile context");
	if (!(seen & 32))
		commands.push_back(SetEyeViews{});
	return commands;
}

// Keeps one block per run of equal values; 32k frames of a still body cost one copy.
template<typename T>
static void Share(std::shared_ptr<const T>& value, const std::shared_ptr<const T>& previous)
{
	if (!value)
		return;
	if (previous && previous != value && Same(*previous, *value))
		value = previous;
}

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

static std::size_t ProfileBytes(const InputSample& sample)
{
	std::size_t bytes = 0;
	for (const auto& controls : sample.controls)
		bytes += controls.physicalProfile.capacity() + 1;
	return bytes;
}

InputHistory::InputHistory(std::size_t capacity, std::size_t byteBudget) : m_byteBudget(byteBudget)
{
	if (byteBudget < sizeof(InputSample) * 2)
		throw std::invalid_argument("Input history budget is too small");
	// Leave at least half the budget for variable device data.
	m_samples.resize(std::min(std::max<std::size_t>(capacity, 1), byteBudget / (2 * sizeof(InputSample))));
	m_bytes = m_samples.capacity() * sizeof(InputSample);
}

const InputSample* InputHistory::Find(std::uint64_t sequence) const noexcept
{
	if (!sequence || sequence >= m_next || m_next - sequence > m_size)
		return nullptr;
	return &m_samples[sequence % m_samples.size()];
}

std::size_t InputHistory::RetainedBytes() const noexcept
{
	return m_bytes;
}

std::size_t InputHistory::ByteBudget() const noexcept
{
	return m_byteBudget;
}

std::uint64_t InputHistory::Append(InputSample sample)
{
	if (sample.trackers && sample.trackers->size() > MaxTrackerDevices)
		throw std::invalid_argument("Tracker inventory exceeds capacity");
	if (m_next == std::numeric_limits<std::uint64_t>::max())
		throw std::overflow_error("Input history sequence exhausted");
	if (m_size)
	{
		const auto& previous = m_samples[(m_next - 1) % m_samples.size()];
		Share(sample.body, previous.body);
		Share(sample.bodyObservation, previous.bodyObservation);
		Share(sample.trackers, previous.trackers);
		Share(sample.context, previous.context);
	}
	const std::array blocks{
		std::pair<const void*, std::size_t>{sample.body.get(), sizeof(BodyControls)},
		std::pair<const void*, std::size_t>{sample.bodyObservation.get(), sizeof(BodyObservation)},
		std::pair<const void*, std::size_t>{
			sample.trackers.get(),
			sizeof(TrackerDevices) + (sample.trackers ? sample.trackers->capacity() * sizeof(TrackerDevice) : 0)},
		std::pair<const void*, std::size_t>{
			sample.context.get(), sizeof(std::vector<std::byte>) + (sample.context ? sample.context->capacity() : 0)}};
	// Prepare ledger allocations before changing the ring; failed admission leaves it intact.
	std::size_t retained = 0;
	try
	{
		for (const auto& [identity, bytes] : blocks)
		{
			Retain(identity, bytes);
			++retained;
		}
	}
	catch (...)
	{
		for (std::size_t index = 0; index < retained; ++index)
			Release(blocks[index].first);
		throw;
	}
	m_bytes += ProfileBytes(sample);
	while (m_size && (m_size == m_samples.size() || m_bytes > m_byteBudget))
		Evict();
	static_assert(std::is_nothrow_move_assignable_v<InputSample>);
	const auto sequence = m_next++;
	if (m_bytes > m_byteBudget)
	{
		// A single record cannot fit. Keep its sequence unavailable, rather than exceed the budget.
		ReleaseSample(sample);
		return sequence;
	}
	m_samples[sequence % m_samples.size()] = std::move(sample);
	++m_size;
	return sequence;
}

void InputHistory::Clear() noexcept
{
	// Sequence numbers keep rising so stale references never resolve.
	m_size = 0;
	std::fill(m_samples.begin(), m_samples.end(), InputSample{});
	m_blocks.clear();
	m_bytes = m_samples.capacity() * sizeof(InputSample);
}

void InputHistory::Retain(const void* identity, std::size_t bytes)
{
	if (!identity)
		return;
	// Include an allowance for the shared control block and ordered-map node.
	const auto [entry, inserted] = m_blocks.try_emplace(identity, Block{bytes + 128, 0});
	if (inserted)
		m_bytes += entry->second.bytes;
	++entry->second.references;
}

void InputHistory::Release(const void* identity) noexcept
{
	if (!identity)
		return;
	const auto entry = m_blocks.find(identity);
	if (--entry->second.references == 0)
	{
		m_bytes -= entry->second.bytes;
		m_blocks.erase(entry);
	}
}

void InputHistory::ReleaseSample(const InputSample& sample) noexcept
{
	Release(sample.body.get());
	Release(sample.bodyObservation.get());
	Release(sample.trackers.get());
	Release(sample.context.get());
	m_bytes -= ProfileBytes(sample);
}

void InputHistory::Evict() noexcept
{
	auto& oldest = m_samples[(m_next - m_size) % m_samples.size()];
	ReleaseSample(oldest);
	oldest = {};
	--m_size;
}

automation::InputRecording TakeFromInput(const std::vector<const InputSample*>& samples)
{
	if (samples.size() < 2)
		throw std::invalid_argument("Select at least two frames with input");
	if (std::ranges::find(samples, nullptr) != samples.end())
		throw std::invalid_argument("Some input in this range is no longer saved.");
	const auto& origin = *samples.front();
	for (const auto* sample : samples)
	{
		if (sample->identity.session != origin.identity.session || sample->identity.run != origin.identity.run ||
			sample->seed != origin.seed || sample->epoch != origin.epoch)
			throw std::invalid_argument("The range spans more than one run");
		if (sample->trackers && sample->trackers->size() > MaxTrackerDevices)
			throw std::invalid_argument("Tracker inventory exceeds capacity");
		if (!sample->context)
			throw std::invalid_argument("This run did not record its view/profile context");
		if (!sample->complete || !sample->body || !sample->trackers || !sample->gaze)
			throw std::invalid_argument("Some device input is missing. Choose a range with complete input.");
	}
	automation::InputRecording take;
	take.version =
		std::ranges::any_of(samples, [](const InputSample* sample) { return bool(sample->bodyObservation); }) ? 4 : 3;
	take.seed = origin.seed;
	take.originFrame = FrameId{origin.identity.frame};
	take.originRuntimeTime = origin.runtimeTime;
	take.initialScenarios = origin.scenarios;
	take.initialScenarios[static_cast<std::size_t>(FailureScenario::Recenter)] = false;
	auto& initial = take.initial;
	initial.push_back(SetHeadPose{origin.head});
	for (std::uint32_t hand = 0; hand < 2; ++hand)
	{
		initial.push_back(SetControllerPose{hand, origin.poses[hand]});
		initial.push_back(SetControllerControls{hand, origin.controls[hand]});
	}
	initial.push_back(BodyCommand(origin));
	initial.push_back(SetGaze{*origin.gaze});
	initial.push_back(SetTrackers{*origin.trackers});
	for (auto& command : ContextCommands(origin))
		initial.push_back(std::move(command));

	// Keep every input transition; thinning a take can drop a button press or release.
	std::vector<std::size_t> changed;
	for (std::size_t index = 1; index < samples.size(); ++index)
	{
		const auto& a = *samples[index - 1];
		const auto& b = *samples[index];
		if (b.identity.frame <= a.identity.frame)
			throw std::invalid_argument("The range is not in frame order");
		if (b.identity.frame - a.identity.frame != 1)
			throw std::invalid_argument("The range has missing input frames. Select a continuous range");
		if (a.scenarios != b.scenarios || !Same(a.head, b.head) || !Same(a.poses, b.poses) ||
			!Same(a.controls, b.controls) || !Same(*a.gaze, *b.gaze) || !Same(a.bodyRoot, b.bodyRoot, SamePose) ||
			(a.body != b.body && !Same(*a.body, *b.body)) || !SameObservation(a, b) ||
			(a.trackers != b.trackers && !Same(*a.trackers, *b.trackers)) ||
			(a.context != b.context && *a.context != *b.context))
			changed.push_back(index);
	}
	const auto tail = changed.empty() || changed.back() != samples.size() - 1 ? 1u : 0u;
	if (changed.size() + tail > 1024)
		throw std::invalid_argument("The take exceeds 1024 input changes. Select a shorter range");
	const InputSample* previous = &origin;
	for (std::size_t position = 0; position < changed.size(); ++position)
	{
		const auto& sample = *samples[changed[position]];
		automation::RecordedTransaction transaction;
		transaction.id = take.transactions.size() + 1;
		transaction.phase = "applied";
		transaction.frame = FrameId{sample.identity.frame};
		transaction.runtimeTime = sample.runtimeTime;
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
			(previous->body != sample.body && !Same(*previous->body, *sample.body)) ||
			!SameObservation(*previous, sample))
			transaction.commands.push_back(BodyCommand(sample));
		if (!Same(*previous->trackers, *sample.trackers))
			transaction.commands.push_back(SetTrackers{*sample.trackers});
		const auto recenter = static_cast<std::size_t>(FailureScenario::Recenter);
		const bool recentered = sample.scenarios[recenter] && !previous->scenarios[recenter];
		// Frame context already contains the recenter result. Recomputing it from
		// the final head pose can change an origin established earlier in the frame.
		if (recentered || (previous->context != sample.context && *previous->context != *sample.context))
			for (auto& command : ContextCommands(sample))
				transaction.commands.push_back(std::move(command));
		for (std::size_t index = 0; index < sample.scenarios.size(); ++index)
			if (index != recenter && previous->scenarios[index] != sample.scenarios[index])
				transaction.commands.push_back(
					SetScenario{static_cast<FailureScenario>(index), sample.scenarios[index]});
		previous = &sample;
		if (transaction.commands.empty())
			continue;
		take.transactions.push_back(std::move(transaction));
	}
	// The take lasts to the range's end, so a still tail is replayed too
	// instead of releasing the devices early.
	const auto& last = *samples.back();
	if (take.transactions.empty() || take.transactions.back().frame.value != last.identity.frame)
	{
		automation::RecordedTransaction transaction;
		transaction.id = take.transactions.size() + 1;
		transaction.phase = "applied";
		transaction.frame = FrameId{last.identity.frame};
		transaction.runtimeTime = last.runtimeTime;
		transaction.commands.push_back(SetHeadPose{last.head});
		take.transactions.push_back(std::move(transaction));
	}
	take.total = take.transactions.size();
	return take;
}
} // namespace simxr::app
