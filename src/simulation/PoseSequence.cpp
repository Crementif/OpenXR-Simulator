#include "simulation/PoseSequence.h"
#include "Pch.h"

namespace simxr
{
PoseSequence::PoseSequence(std::vector<PoseSequenceStep> steps, bool loop) : m_steps(std::move(steps)), m_loop(loop)
{
	if (m_steps.size() > MaxSteps)
		throw std::length_error("Pose sequence exceeds its step limit");
	std::stable_sort(m_steps.begin(), m_steps.end(),
					 [](const auto& left, const auto& right)
					 {
						 return left.frame < right.frame;
					 });
}

std::size_t PoseSequence::Size() const noexcept
{
	return m_steps.size();
}

std::span<const PoseSequenceStep> PoseSequence::Steps() const noexcept
{
	return m_steps;
}

bool PoseSequence::Loops() const noexcept
{
	return m_loop;
}

void PoseSequencePlayback::Start(PoseSequence sequence, FrameId origin)
{
	Start(std::make_shared<const PoseSequence>(std::move(sequence)), origin);
}

void PoseSequencePlayback::Start(std::shared_ptr<const PoseSequence> sequence, FrameId origin)
{
	if (sequence && sequence->Size() &&
		sequence->Steps().back().frame.value > std::numeric_limits<std::uint64_t>::max() - origin.value)
		throw std::overflow_error("Pose sequence extends beyond the frame counter");
	m_sequence = std::move(sequence);
	m_origin = origin;
	m_next = 0;
}

std::span<const PoseSequenceStep> PoseSequencePlayback::Advance(FrameId submitted) noexcept
{
	const auto steps = m_sequence ? m_sequence->Steps() : std::span<const PoseSequenceStep>{};
	if (steps.empty() || submitted < m_origin)
		return {};
	auto elapsed = submitted.value - m_origin.value;
	if (m_sequence->Loops() && elapsed > steps.back().frame.value)
	{
		// The strict comparison proves last+1 fits even at the uint64 boundary.
		elapsed %= steps.back().frame.value + 1;
		m_origin = {submitted.value - elapsed};
		m_next = 0;
	}
	const auto first = m_next;
	while (m_next < steps.size() && steps[m_next].frame.value <= elapsed)
		++m_next;
	return steps.subspan(first, m_next - first);
}

std::optional<FrameId> PoseSequencePlayback::NextFrame() const noexcept
{
	const auto steps = m_sequence ? m_sequence->Steps() : std::span<const PoseSequenceStep>{};
	if (steps.empty())
		return {};
	std::uint64_t offset = 0;
	if (m_next < steps.size())
		offset = steps[m_next].frame.value;
	else
	{
		if (!m_sequence->Loops() || steps.back().frame.value == std::numeric_limits<std::uint64_t>::max())
			return {};
		const auto cycle = steps.back().frame.value + 1;
		if (steps.front().frame.value > std::numeric_limits<std::uint64_t>::max() - cycle)
			return {};
		offset = cycle + steps.front().frame.value;
	}
	if (offset > std::numeric_limits<std::uint64_t>::max() - m_origin.value)
		return {};
	return FrameId{m_origin.value + offset};
}

bool PoseSequencePlayback::Complete() const noexcept
{
	return !m_sequence || !m_sequence->Size() || (!m_sequence->Loops() && m_next == m_sequence->Size());
}
} // namespace simxr
