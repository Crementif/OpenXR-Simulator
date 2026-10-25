#include "simulation/PoseSequenceExecution.h"
#include "Pch.h"
#include "math/Math.h"

#include <cmath>
#include <stdexcept>
#include <tuple>

namespace simxr
{
template <class T, std::size_t N> static bool Any(const std::array<std::optional<T>, N>& values)
{
	return std::any_of(values.begin(), values.end(),
					   [](const auto& value)
					   {
						   return value.has_value();
					   });
}

static void Position(XrVector3f& value, const std::array<std::optional<float>, 3>& patch)
{
	value = {patch[0].value_or(value.x), patch[1].value_or(value.y), patch[2].value_or(value.z)};
}

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
	std::copy_n(inherited.begin(), m_channels.size(), m_channels.begin());
	for (const auto& step : m_program->Steps())
	{
		if (step.head && (Any(step.head->position) || Any(step.head->angles)))
			m_channels[0] = m_channels[1] = m_channels[2] = true;
		for (std::size_t hand = 0; hand < 2; ++hand)
			if (const auto& patch = step.controllers[hand])
			{
				m_channels[hand + 1] = m_channels[hand + 1] || Any(patch->position) || patch->orientation.has_value();
				m_channels[hand + 3] = m_channels[hand + 3] || patch->trigger || patch->grip || Any(patch->stick) ||
									   patch->primary || patch->secondary;
			}
	}
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

void PoseSequenceExecution::Patch(const PoseSequenceStep& step)
{
	if (step.head)
	{
		Position(m_head.pose.position, step.head->position);
		if (Any(step.head->angles))
		{
			for (std::size_t axis = 0; axis < m_angles.size(); ++axis)
				m_angles[axis] = step.head->angles[axis].value_or(m_angles[axis]);
			m_head.pose.orientation = EulerAngles(m_angles[0], m_angles[1], m_angles[2]);
		}
	}
	for (std::size_t hand = 0; hand < 2; ++hand)
	{
		if (const auto& patch = step.controllers[hand])
		{
			if (m_channels[hand + 1])
			{
				Position(m_local[hand].position, patch->position);
				if (patch->orientation)
					m_local[hand].orientation = *patch->orientation;
			}
			auto& controls = m_controls[hand];
			controls.trigger = std::clamp(patch->trigger.value_or(controls.trigger), 0.0f, 1.0f);
			controls.grip = std::clamp(patch->grip.value_or(controls.grip), 0.0f, 1.0f);
			controls.stick.x = std::clamp(patch->stick[0].value_or(controls.stick.x), -1.0f, 1.0f);
			controls.stick.y = std::clamp(patch->stick[1].value_or(controls.stick.y), -1.0f, 1.0f);
			controls.primary = patch->primary.value_or(controls.primary);
			controls.secondary = patch->secondary.value_or(controls.secondary);
			if (patch->trigger)
				controls.triggerTouch = controls.trigger > 0;
			if (patch->grip)
				controls.gripForce = controls.grip;
			if (patch->stick[0])
				controls.trackpad.x = controls.stick.x;
			if (patch->stick[1])
				controls.trackpad.y = controls.stick.y;
			if (patch->primary)
				controls.primaryTouch = controls.primary;
			if (patch->secondary)
				controls.secondaryTouch = controls.secondary;
		}
	}
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
		Patch(step);
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

CommandSource PoseSequenceExecution::Source() const noexcept
{
	return {CommandSourceKind::Sequence, m_status.id};
}
} // namespace simxr
