#pragma once

#include "platform/Clock.h"

#include <memory>

namespace simxr
{
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
}
