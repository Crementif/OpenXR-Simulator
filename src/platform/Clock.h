#pragma once

#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>

namespace simxr
{
using HostTime = std::chrono::steady_clock::time_point;

class Clock
{
public:
	virtual ~Clock() = default;
	virtual HostTime Now() const = 0;
	virtual void WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime deadline) = 0;
	virtual void Wake()
	{
	}
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

	void WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime deadline) override
	{
		changed.wait_until(lock, deadline);
	}
};
}
