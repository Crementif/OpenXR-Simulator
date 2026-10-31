#include "graphics/ApplicationGpuTimer.h"
#include "Pch.h"

namespace simxr
{
ApplicationGpuTimer::~ApplicationGpuTimer()
{
	StopCollecting();
}

bool ApplicationGpuTimer::StartCollecting() noexcept
{
	try
	{
		m_collector = std::jthread([this](std::stop_token stop) {
			std::unique_lock lock(m_mutex);
			while (!stop.stop_requested())
			{
				m_changed.wait(lock, stop, [this] { return Pending(); });
				if (stop.stop_requested())
					break;
				CollectLocked();
				if (Pending())
					m_changed.wait_for(lock, stop, std::chrono::milliseconds(2), [] { return false; });
			}
		});
		return true;
	}
	catch (...)
	{
		return false;
	}
}

void ApplicationGpuTimer::StopCollecting() noexcept
{
	m_collector.request_stop();
	if (m_collector.joinable())
		m_collector.join();
}

void ApplicationGpuTimer::Begin() noexcept
{
	std::lock_guard lock(m_mutex);
	// A second successful begin discards the old frame, but its native work must retire.
	EndLocked();
	CollectLocked();
	m_ended = {};
	for (std::size_t index = 0; index < m_slots.size(); ++index)
	{
		auto& slot = m_slots[index];
		if (slot.state != State::Free)
			continue;
		try
		{
			slot.duration = GpuDuration::Create();
			slot.state = State::Failed;
			if (Write(index, false))
			{
				slot.state = State::Open;
				m_open = index;
			}
		}
		catch (...)
		{}
		return;
	}
}

GpuDuration ApplicationGpuTimer::End() noexcept
{
	std::lock_guard lock(m_mutex);
	return EndLocked();
}

void ApplicationGpuTimer::Collect() noexcept
{
	std::lock_guard lock(m_mutex);
	CollectLocked();
}

GpuDuration ApplicationGpuTimer::EndLocked() noexcept
{
	if (m_open == SlotCount)
		return m_ended;
	const auto index = std::exchange(m_open, SlotCount);
	auto& slot = m_slots[index];
	slot.state = State::Failed;
	try
	{
		if (Write(index, true))
			slot.state = State::Pending;
	}
	catch (...)
	{}
	m_changed.notify_one();
	m_ended = slot.duration;
	return m_ended;
}

void ApplicationGpuTimer::CollectLocked() noexcept
{
	for (std::size_t index = 0; index < m_slots.size(); ++index)
	{
		auto& slot = m_slots[index];
		if (slot.state != State::Pending)
			continue;
		try
		{
			std::int64_t nanoseconds = -1;
			if (Read(index, nanoseconds))
			{
				slot.duration.Store(nanoseconds);
				slot = {};
			}
		}
		catch (...)
		{
			slot.state = State::Failed;
		}
	}
}

bool ApplicationGpuTimer::Pending() const noexcept
{
	return std::any_of(m_slots.begin(), m_slots.end(), [](const Slot& slot) { return slot.state == State::Pending; });
}
}
