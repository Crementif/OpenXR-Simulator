#include "graphics/ApplicationGpuTimer.h"
#include "Pch.h"

namespace simxr
{
void ApplicationGpuTimer::Begin() noexcept
{
	// A second successful begin discards the old frame, but its native work must retire.
	End();
	Collect();
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
	if (m_open == SlotCount)
		return {};
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
	return slot.duration;
}

void ApplicationGpuTimer::Collect() noexcept
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
}
