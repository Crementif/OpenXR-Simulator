#include "simulation/PoseSequenceExecution.h"
#include "Pch.h"
#include "math/Math.h"

#include <tuple>

namespace simxr
{
static bool SamePose(const XrPosef& left, const XrPosef& right)
{
	return std::tie(left.position.x, left.position.y, left.position.z, left.orientation.x, left.orientation.y,
					left.orientation.z, left.orientation.w) ==
		   std::tie(right.position.x, right.position.y, right.position.z, right.orientation.x, right.orientation.y,
					right.orientation.z, right.orientation.w);
}

static std::array<float, 3> Angles(const XrQuaternionf& q)
{
	const auto pitch = std::asin(std::clamp(2 * (q.w * q.x - q.y * q.z), -1.0f, 1.0f));
	if (std::abs(std::cos(pitch)) < 0.00001f)
		return {std::atan2(2 * (q.w * q.y - q.x * q.z), 1 - 2 * (q.y * q.y + q.z * q.z)), pitch, 0};
	return {std::atan2(2 * (q.x * q.z + q.w * q.y), 1 - 2 * (q.x * q.x + q.y * q.y)), pitch,
			std::atan2(2 * (q.x * q.y + q.w * q.z), 1 - 2 * (q.x * q.x + q.z * q.z))};
}

PoseSequenceExecution::PoseSequenceExecution(std::uint64_t id, CommandSource owner,
											 std::shared_ptr<const PoseSequence> program,
											 std::array<bool, ControlChannelCount> inherited)
	: m_program(std::move(program))
{
	if (!id || !owner.id || !m_program)
		throw std::invalid_argument("Invalid pose sequence execution");
	m_status.id = id;
	m_status.owner = owner;
	m_status.loop = m_program->Loops();
	m_status.timed = m_program->Timed();
	for (std::size_t index = 0; index < m_channels.size(); ++index)
		m_channels[index] = inherited[index] || m_program->Channels()[index];
}

PoseSequenceFrame PoseSequenceExecution::Prepare(FrameId frame, SimulationTime time,
												 const CommandConfiguration& initial)
{
	PoseSequenceFrame result;
	if (!CanPrepare())
		return result;
	const bool starting = !m_started;
	if (starting)
		Initialize(frame, time, initial);
	if (const auto next = m_playback.NextFrame(); !m_status.timed && next && *next < frame)
	{
		Accept(frame, time, CommandResult::Late);
		return result;
	}
	// Completed programs retain their local offsets until ownership is released.
	bool headChanged = false;
	if (!m_channels[0] && (m_channels[1] || m_channels[2]))
	{
		const auto head = NormalizedPose(initial.tracking.head.pose);
		if (!head)
			throw std::invalid_argument("Invalid sequence head pose");
		headChanged = !SamePose(m_head.pose, *head);
		m_head.pose = *head;
	}
	bool advanced = false;
	if (m_status.active && m_status.timed)
	{
		AdvanceTime(time, result.capture);
		advanced = true;
	}
	else if (m_status.active)
		for (const auto& step : m_playback.Advance(frame))
		{
			ApplyStep(step, result.capture);
			advanced = true;
		}
	result.advanced = starting || advanced || headChanged;
	if (result.advanced)
	{
		for (std::size_t hand = 0; hand < m_controllers.size(); ++hand)
			if (m_channels[hand + 1])
				m_controllers[hand].pose.pose = Compose(m_head.pose, m_local[hand]);
		result.commands = Commands();
	}
	return result;
}

void PoseSequenceExecution::Accept(FrameId frame, SimulationTime time, CommandResult result) noexcept
{
	m_status.result = result;
	m_status.lastFrame = frame;
	m_status.lastTime = time;
	m_status.active = result == CommandResult::Applied && !(m_status.timed ? m_finished : m_playback.Complete());
	if (result == CommandResult::Applied)
		++m_status.groups;
	if (!m_status.active)
	{
		m_program.reset();
		m_playback = {};
	}
}

void PoseSequenceExecution::Cancel() noexcept
{
	m_status.active = false;
	m_status.result = CommandResult::Cancelled;
	m_program.reset();
	m_playback = {};
}

bool PoseSequenceExecution::CanPrepare() const noexcept
{
	return m_status.result == CommandResult::Queued || m_status.result == CommandResult::Applied;
}

bool PoseSequenceExecution::HasControl(const std::array<bool, ControlChannelCount>& owned) const noexcept
{
	for (std::size_t index = 0; index < m_channels.size(); ++index)
		if (m_channels[index] && !owned[index])
			return false;
	return true;
}

const PoseSequenceStatus& PoseSequenceExecution::Status() const noexcept
{
	return m_status;
}

CommandSource PoseSequenceExecution::Source() const noexcept
{
	return {CommandSourceKind::Sequence, m_status.id};
}

std::shared_ptr<const PoseSequence> PoseSequenceExecution::Program() const noexcept
{
	return m_program;
}

void PoseSequenceExecution::Initialize(FrameId frame, SimulationTime time, const CommandConfiguration& initial)
{
	if (!m_status.timed)
		m_playback.Start(m_program, frame);
	m_status.origin = frame;
	m_status.originTime = time;
	m_head = initial.tracking.head;
	m_controllers = initial.tracking.controllers;
	m_controls = initial.input.controllers;
	if (m_channels[0] || m_channels[1] || m_channels[2])
	{
		const auto head = NormalizedPose(m_head.pose);
		if (!head)
			throw std::invalid_argument("Invalid initial sequence head pose");
		m_head.pose = *head;
		m_angles = Angles(head->orientation);
		for (std::size_t hand = 0; hand < 2; ++hand)
		{
			if (!m_channels[hand + 1])
				continue;
			const auto controller = NormalizedPose(m_controllers[hand].pose.pose);
			if (!controller)
				throw std::invalid_argument("Invalid initial sequence controller pose");
			m_local[hand] = RelativePose(*controller, *head);
		}
	}
	const auto& head = m_head.pose.position;
	std::copy_n(std::array{head.x, head.y, head.z, m_angles[0], m_angles[1], m_angles[2]}.begin(), 6,
				m_cycleStart.scalars.begin());
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		const auto& local = m_local[hand].position;
		const auto& controls = m_controls[hand];
		std::copy_n(
			std::array{local.x, local.y, local.z, controls.trigger, controls.grip, controls.stick.x, controls.stick.y}
				.begin(),
			7, m_cycleStart.scalars.begin() + 6 + 7 * hand);
		m_cycleStart.orientations[hand] = m_local[hand].orientation;
	}
	m_started = true;
}

void PoseSequenceExecution::ApplyStep(const PoseSequenceStep& step, bool& capture)
{
	if (step.head)
		step.head->Apply(m_head.pose, m_angles);
	for (std::size_t hand = 0; hand < step.controllers.size(); ++hand)
		if (step.controllers[hand])
			step.controllers[hand]->Apply(m_local[hand], m_controls[hand]);
	capture = capture || step.capture;
}

void PoseSequenceExecution::AdvanceTime(SimulationTime time, bool& capture)
{
	const auto& program = *m_program;
	const auto steps = program.Steps();
	const auto length = program.Length();
	auto elapsed = time.value - m_status.originTime.value;
	if (program.Loops() && length > Duration{})
	{
		// Rows of skipped cycles still fire once so buttons and captures are never lost.
		if (const auto cycle = static_cast<std::uint64_t>(elapsed / length); cycle != m_cycle)
		{
			for (; m_nextStep < steps.size(); ++m_nextStep)
				ApplyStep(steps[m_nextStep], capture);
			m_cycleStart = program.End(m_cycleStart);
			m_cycle = cycle;
			m_nextStep = 0;
		}
		elapsed %= length;
	}
	for (; m_nextStep < steps.size() && steps[m_nextStep].time <= elapsed; ++m_nextStep)
		ApplyStep(steps[m_nextStep], capture);
	const auto sample = program.Sample(elapsed, m_cycleStart);
	sample.head.Apply(m_head.pose, m_angles);
	for (std::size_t hand = 0; hand < 2; ++hand)
		sample.controllers[hand].Apply(m_local[hand], m_controls[hand]);
	m_finished = (!program.Loops() || length == Duration{}) && m_nextStep == steps.size() && elapsed >= length;
}

std::vector<SimulationCommand> PoseSequenceExecution::Commands() const
{
	std::vector<SimulationCommand> commands;
	commands.reserve(10);
	for (std::size_t index = 0; index < m_channels.size(); ++index)
		if (m_channels[index])
			commands.push_back(TakeControl{static_cast<ControlChannel>(index)});
	if (m_channels[0])
		commands.push_back(SetHeadPose{m_head});
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		if (m_channels[hand + 1])
			commands.push_back(SetControllerPose{static_cast<std::uint32_t>(hand), m_controllers[hand]});
		if (m_channels[hand + 3])
			commands.push_back(SetControllerControls{static_cast<std::uint32_t>(hand), m_controls[hand]});
	}
	return commands;
}

} // namespace simxr
