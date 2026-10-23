#include "platform/windows/WindowsClock.h"
#include "Pch.h"

#include <limits>
#include <stdexcept>
#include <windows.h>

namespace simxr
{
static bool ScalePositiveTime(std::int64_t value, std::int64_t numerator, std::int64_t denominator,
							  std::int64_t& output) noexcept
{
	if (value <= 0 || numerator <= 0 || denominator <= 0)
		return false;
	const auto whole = static_cast<std::uint64_t>(value / denominator);
	const auto remainder = static_cast<std::uint64_t>(value % denominator);
	const auto scale = static_cast<std::uint64_t>(numerator);
	const auto maximum = static_cast<std::uint64_t>(std::numeric_limits<std::int64_t>::max());
	if (whole > maximum / scale || remainder > std::numeric_limits<std::uint64_t>::max() / scale)
		return false;
	const auto fractional = remainder * scale / static_cast<std::uint64_t>(denominator);
	const auto integral = whole * scale;
	if (fractional > maximum - integral)
		return false;
	output = static_cast<std::int64_t>(integral + fractional);
	return true;
}

XrResult PerformanceCounterToTime(std::int64_t counter, XrTime& time) noexcept
{
	LARGE_INTEGER frequency{};
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
		return XR_ERROR_RUNTIME_FAILURE;
	return ScalePositiveTime(counter, 1000000000, frequency.QuadPart, time) ? XR_SUCCESS : XR_ERROR_TIME_INVALID;
}

XrResult TimeToPerformanceCounter(XrTime time, std::int64_t& counter) noexcept
{
	LARGE_INTEGER frequency{};
	if (!QueryPerformanceFrequency(&frequency) || frequency.QuadPart <= 0)
		return XR_ERROR_RUNTIME_FAILURE;
	return ScalePositiveTime(time, frequency.QuadPart, 1000000000, counter) ? XR_SUCCESS : XR_ERROR_TIME_INVALID;
}

XrResult ReadPerformanceTime(XrTime& time) noexcept
{
	LARGE_INTEGER counter{};
	if (!QueryPerformanceCounter(&counter))
		return XR_ERROR_RUNTIME_FAILURE;
	return PerformanceCounterToTime(counter.QuadPart, time);
}

struct WindowsClock::State
{
	HANDLE timer = CreateWaitableTimerExW(nullptr, nullptr, 0x00000002, TIMER_ALL_ACCESS);
	HANDLE wake = CreateEventW(nullptr, FALSE, FALSE, nullptr);

	~State()
	{
		if (timer)
			CloseHandle(timer);
		if (wake)
			CloseHandle(wake);
	}
};

WindowsClock::WindowsClock() : m_state(std::make_unique<State>()) {}

WindowsClock::~WindowsClock() = default;

HostTime WindowsClock::Now() const
{
	return std::chrono::steady_clock::now();
}

void WindowsClock::WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime deadline)
{
	const auto now = Now();
	if (deadline <= now)
		return;
	const auto remaining = HostTimeDifference(deadline, now);
	using TimerDuration = std::chrono::duration<LONGLONG, std::ratio<1, 10000000>>;
	LARGE_INTEGER due{};
	due.QuadPart = -std::chrono::ceil<TimerDuration>(remaining).count();
	if (!m_state->timer || !m_state->wake || !SetWaitableTimer(m_state->timer, &due, 0, nullptr, nullptr, FALSE))
	{
		changed.wait_until(lock, deadline);
		return;
	}
	HANDLE handles[]{m_state->wake, m_state->timer};
	lock.unlock();
	const auto result = WaitForMultipleObjects(2, handles, FALSE, INFINITE);
	lock.lock();
	if (result == WAIT_FAILED)
		throw std::runtime_error("Frame timer wait failed");
}

void WindowsClock::Wake()
{
	if (m_state->wake)
		SetEvent(m_state->wake);
}

std::shared_ptr<Clock> WindowsClock::IndependentWaiter() const
{
	return std::make_shared<WindowsClock>();
}
} // namespace simxr
