#pragma once

namespace simxr
{
enum class CompletionStatus
{
	Pending,
	Complete,
	Failed
};

class GpuCompletion
{
public:
	virtual ~GpuCompletion() = default;
	// Observation is thread-safe and nonblocking, with terminal Complete/Failed.
	// A failure does not establish GPU idleness or authorize native destruction.
	virtual CompletionStatus PollCompletion() const noexcept = 0;
};
} // namespace simxr
