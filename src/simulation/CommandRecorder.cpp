#include "simulation/CommandRecorder.h"
#include "Pch.h"

namespace simxr
{
CommandRecorder::CommandRecorder(std::size_t capacity) : m_capacity(capacity)
{
	if (!capacity || capacity > 4096)
		throw std::invalid_argument("Invalid command recording capacity");
}

void CommandRecorder::Start(const CommandRecordingOrigin& origin, bool drain)
{
	std::lock_guard mutation(m_mutationMutex);
	CommandRecording prepared;
	prepared.active = true;
	prepared.drain = drain;
	prepared.origin = origin;
	if (origin.configuration.tracking.bodyObservation)
		prepared.version = 4;
	std::vector<std::optional<CommandOutcome>> slots(m_capacity);
	std::lock_guard lock(m_mutex);
	if (m_state.active)
		throw std::logic_error("Command recording is already active");
	if (m_state.identity == std::numeric_limits<std::uint64_t>::max())
		throw std::overflow_error("Recording identities exhausted");
	prepared.identity = m_state.identity + 1;
	m_state = std::move(prepared);
	m_slots.swap(slots);
}

void CommandRecorder::Acknowledge(std::size_t through)
{
	std::lock_guard mutation(m_mutationMutex);
	std::lock_guard lock(m_mutex);
	if (!m_state.drain || through > m_state.total)
		throw std::invalid_argument("Invalid recording acknowledgement");
	while (m_state.floor < through)
	{
		m_slots[m_state.floor % m_slots.size()].reset();
		++m_state.floor;
	}
}

void CommandRecorder::Stop()
{
	std::lock_guard mutation(m_mutationMutex);
	std::lock_guard lock(m_mutex);
	m_state.active = false;
}

void CommandRecorder::Fail()
{
	std::lock_guard mutation(m_mutationMutex);
	std::lock_guard lock(m_mutex);
	if (m_state.active)
	{
		m_state.active = false;
		m_state.overflow = true;
	}
}

CommandRecording CommandRecorder::Snapshot(std::optional<std::size_t> offset, std::size_t count) const
{
	if (count > 256)
		throw std::invalid_argument("Command recording page exceeds 256 entries");
	std::lock_guard lock(m_mutex);
	const auto start = offset.value_or(m_state.floor);
	if (start < m_state.floor)
		throw std::out_of_range("Recording offset has already been acknowledged");
	CommandRecording result = m_state;
	const auto begin = std::min(start, m_state.total);
	const auto length = std::min(count, m_state.total - begin);
	result.transactions.reserve(length);
	for (std::size_t index = begin; index < begin + length; ++index)
		result.transactions.push_back(*m_slots[index % m_slots.size()]);
	return result;
}

bool CommandRecorder::Observe(const CommandOutcome& outcome)
{
	return Observe(std::span{&outcome, 1});
}

bool CommandRecorder::Observe(std::span<const CommandOutcome> outcomes, const std::function<void()>& commit)
{
	return ObservePrepared(outcomes, [&](bool) {
		if (commit)
			commit();
	});
}

bool CommandRecorder::ObservePrepared(std::span<const CommandOutcome> outcomes, const std::function<void(bool)>& commit)
{
	std::lock_guard mutation(m_mutationMutex);
	std::vector<CommandOutcome> prepared;
	bool active = false, overflow = false;
	std::size_t count = 0;
	std::uint32_t version = 3;
	{
		std::lock_guard lock(m_mutex);
		active = m_state.active;
		version = m_state.version;
		overflow = m_state.overflow;
		if (active)
		{
			const auto remaining = m_slots.size() - (m_state.total - m_state.floor);
			count = std::min({remaining, outcomes.size(), std::numeric_limits<std::size_t>::max() - m_state.total});
			overflow = count < outcomes.size();
		}
	}
	// Prepare only new outcomes. Reservation failure leaves retained entries unchanged.
	prepared.assign(outcomes.begin(), outcomes.begin() + count);
	for (auto& outcome : prepared)
	{
		outcome.recordingOverrun = overflow;
		for (const auto& command : outcome.transaction.commands)
			if (const auto* body = std::get_if<SetBodyControls>(&command); body && body->observation)
				version = 4;
	}
	if (commit)
		commit(!overflow);
	if (active)
	{
		static_assert(std::is_nothrow_move_constructible_v<CommandOutcome>);
		std::lock_guard lock(m_mutex);
		for (auto& outcome : prepared)
			m_slots[m_state.total++ % m_slots.size()].emplace(std::move(outcome));
		m_state.version = version;
		m_state.active = !overflow;
		m_state.overflow = overflow;
	}
	return !overflow;
}
} // namespace simxr
