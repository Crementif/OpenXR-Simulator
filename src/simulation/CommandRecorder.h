#pragma once

#include "simulation/CommandTypes.h"
#include <cstddef>
#include <functional>
#include <mutex>
#include <span>

namespace simxr
{
class CommandRecorder
{
public:
	explicit CommandRecorder(std::size_t capacity = 1024);

	void Start(const CommandRecordingOrigin& origin, bool drain = false);
	// Retire the exclusive prefix only after the consumer has persisted it.
	void Acknowledge(std::size_t through);
	void Stop();
	void Fail();

	CommandRecording Snapshot(std::optional<std::size_t> offset = {}, std::size_t count = 128) const;
	// Records normalized acknowledged outcomes, including rejected/cancelled work.
	// Full history stops recording explicitly; no overwrite or silent drop.
	bool Observe(const CommandOutcome& outcome);
	bool Observe(std::span<const CommandOutcome> outcomes, const std::function<void()>& commit = {});
	// Finalizes the prospective recording result before reservation/publication.
	// The callback may read Snapshot(), but must not mutate this recorder. If it
	// throws, neither the recorder nor its overflow state is published.
	bool ObservePrepared(std::span<const CommandOutcome> outcomes, const std::function<void(bool)>& commit);

private:
	const std::size_t m_capacity;
	// Serialize mutations across external reservation; snapshots use only m_mutex.
	std::mutex m_mutationMutex;
	mutable std::mutex m_mutex;
	CommandRecording m_state;
	std::vector<std::optional<CommandOutcome>> m_slots;
};
} // namespace simxr
