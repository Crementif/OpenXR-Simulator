#include "simulation/CommandRecorder.h"
#include "Pch.h"

#include <stdexcept>

namespace simxr
{
CommandRecorder::CommandRecorder(std::size_t capacity) : m_capacity(capacity)
{
	if (!capacity || capacity > 4096)
		throw std::invalid_argument("Invalid command recording capacity");
}
void CommandRecorder::Start(const CommandRecordingOrigin& origin)
{
	std::lock_guard mutation(m_mutationMutex);
	CommandRecording prepared;
	prepared.active = true;
	prepared.origin = origin;
	prepared.transactions.reserve(m_capacity);
	std::lock_guard lock(m_mutex);
	if (m_state.active)
		throw std::logic_error("Command recording is already active");
	m_state = std::move(prepared);
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

CommandRecording CommandRecorder::Snapshot(std::size_t offset, std::size_t count) const
{
	if (count > 256)
		throw std::invalid_argument("Command recording page exceeds 256 entries");
	std::lock_guard lock(m_mutex);
	CommandRecording result;
	result.active = m_state.active;
	result.overflow = m_state.overflow;
	result.origin = m_state.origin;
	result.total = m_state.transactions.size();
	const auto begin = std::min(offset, m_state.transactions.size());
	const auto end = begin + std::min(count, m_state.transactions.size() - begin);
	result.transactions.assign(m_state.transactions.begin() + begin, m_state.transactions.begin() + end);
	return result;
}
bool CommandRecorder::Observe(const CommandOutcome& outcome)
{
	return Observe(std::span{&outcome, 1});
}
bool CommandRecorder::Observe(std::span<const CommandOutcome> outcomes, const std::function<void()>& commit)
{
	return ObservePrepared(outcomes,
						   [&](bool)
						   {
							   if (commit)
								   commit();
						   });
}

bool CommandRecorder::ObservePrepared(std::span<const CommandOutcome> outcomes, const std::function<void(bool)>& commit)
{
	std::lock_guard mutation(m_mutationMutex);
	std::vector<CommandOutcome> prepared;
	bool active = false;
	bool overflow = false;
	{
		std::lock_guard lock(m_mutex);
		active = m_state.active;
		overflow = m_state.overflow;
		if (active)
		{
			prepared = m_state.transactions;
			const auto previousCount = prepared.size();
			const auto remaining = m_capacity - previousCount;
			const auto count = std::min(remaining, outcomes.size());
			prepared.insert(prepared.end(), outcomes.begin(), outcomes.begin() + count);
			overflow = count < outcomes.size();
			for (auto index = previousCount; index < prepared.size(); ++index)
				prepared[index].recordingOverrun = overflow;
		}
	}
	// Do not hold the snapshot mutex while calling external reservation code.
	// Mutation serialization keeps the prepared transaction current until commit.
	if (commit)
		commit(!overflow);
	if (active)
	{
		std::lock_guard lock(m_mutex);
		m_state.transactions.swap(prepared);
		m_state.active = !overflow;
		m_state.overflow = overflow;
	}
	return !overflow;
}
} // namespace simxr
