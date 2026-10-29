#pragma once

#include <chrono>
#include <condition_variable>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>

namespace simxr
{
using HostTime = std::chrono::steady_clock::time_point;

enum class NativeClockDomain : std::uint32_t
{
	MonotonicNanoseconds = 1,
	PerformanceCounterNanoseconds = 2
};

struct NativeClockSample
{
	NativeClockDomain domain;
	HostTime before, after;
	std::int64_t nanoseconds = 0;
	std::int64_t resolutionNanoseconds = 0;
};

// Brackets the native sample with local steady-clock observations. Consumers
// retain this uncertainty; they must not infer clock alignment from IPC latency.
NativeClockSample ReadNativeClockSample();

// Reject an unrepresentable interval before signed chrono subtraction.
HostTime::duration HostTimeDifference(HostTime end, HostTime begin);

class Clock
{
public:
	virtual ~Clock() = default;
	virtual HostTime Now() const = 0;

	virtual std::optional<NativeClockSample> NativeSample() const
	{
		return {};
	}

	virtual void WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime deadline) = 0;

	virtual void Wake() {}

	// Clocks with a single native waiter provide an independent timer/wake owner
	// on the same host time axis. An empty result means concurrent waits are safe.
	virtual std::shared_ptr<Clock> IndependentWaiter() const
	{
		return {};
	}
};

class SteadyClock final : public Clock
{
public:
	HostTime Now() const override
	{
		return std::chrono::steady_clock::now();
	}

	std::optional<NativeClockSample> NativeSample() const override
	{
		return ReadNativeClockSample();
	}

	void WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime deadline) override
	{
		changed.wait_until(lock, deadline);
	}
};
} // namespace simxr
