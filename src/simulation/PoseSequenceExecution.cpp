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
	for (std::size_t index = 0; index < m_channels.size(); ++index)
		m_channels[index] = inherited[index] || m_program->Channels()[index];
}

void PoseSequenceExecution::Initialize(FrameId frame, const CommandConfiguration& initial)
{
	m_playback.Start(m_program, frame);
	m_status.origin = frame;
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
	m_started = true;
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

PoseSequenceFrame PoseSequenceExecution::Prepare(FrameId frame, const CommandConfiguration& initial)
{
	PoseSequenceFrame result;
	if (!CanPrepare())
		return result;
	const bool starting = !m_started;
	if (starting)
		Initialize(frame, initial);
	if (const auto next = m_playback.NextFrame(); next && *next < frame)
	{
		Accept(frame, CommandResult::Late);
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
	const auto steps = m_status.active ? m_playback.Advance(frame) : std::span<const PoseSequenceStep>{};
	for (const auto& step : steps)
	{
		if (step.head)
			step.head->Apply(m_head.pose, m_angles);
		for (std::size_t hand = 0; hand < step.controllers.size(); ++hand)
			if (step.controllers[hand])
				step.controllers[hand]->Apply(m_local[hand], m_controls[hand]);
		result.capture = result.capture || step.capture;
	}
	result.advanced = starting || !steps.empty() || headChanged;
	if (result.advanced)
	{
		for (std::size_t hand = 0; hand < m_controllers.size(); ++hand)
			if (m_channels[hand + 1])
				m_controllers[hand].pose.pose = Compose(m_head.pose, m_local[hand]);
		result.commands = Commands();
	}
	return result;
}

void PoseSequenceExecution::Accept(FrameId frame, CommandResult result) noexcept
{
	m_status.result = result;
	m_status.lastFrame = frame;
	m_status.active = result == CommandResult::Applied && !m_playback.Complete();
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

std::shared_ptr<const PoseSequence> PoseSequenceExecution::Program() const noexcept
{
	return m_program;
}

CommandSource PoseSequenceExecution::Source() const noexcept
{
	return {CommandSourceKind::Sequence, m_status.id};
}
} // namespace simxr
