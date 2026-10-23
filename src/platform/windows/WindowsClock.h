#pragma once

#include "platform/Clock.h"

#include <cstdint>
#include <memory>
#include <openxr/openxr.h>

namespace simxr
{
XrResult PerformanceCounterToTime(std::int64_t counter, XrTime& time) noexcept;
XrResult TimeToPerformanceCounter(XrTime time, std::int64_t& counter) noexcept;
XrResult ReadPerformanceTime(XrTime& time) noexcept;

class WindowsClock final : public Clock
{
  public:
	WindowsClock();
	~WindowsClock() override;
	HostTime Now() const override;
	void WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime deadline) override;
	void Wake() override;
	std::shared_ptr<Clock> IndependentWaiter() const override;

  private:
	struct State;
	std::unique_ptr<State> m_state;
};
} // namespace simxr
