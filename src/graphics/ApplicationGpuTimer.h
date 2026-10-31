#pragma once

#include "graphics/FrameGpuTiming.h"
#include <array>
#include <cstddef>
#include <memory>

namespace simxr
{
// Called under the native adapter's submission gate. A full ring skips timing;
// querying never waits or flushes the application queue.
class ApplicationGpuTimer
{
public:
	virtual ~ApplicationGpuTimer() = default;
	void Begin() noexcept;
	GpuDuration End() noexcept;
	void Collect() noexcept;

	virtual bool Drain(std::uint64_t)
	{
		Collect();
		return true;
	}

protected:
	static constexpr std::size_t SlotCount = 16;
	virtual bool Write(std::size_t slot, bool end) = 0;
	virtual bool Read(std::size_t slot, std::int64_t& nanoseconds) = 0;

private:
	enum class State
	{
		Free,
		Open,
		Pending,
		Failed
	};

	struct Slot
	{
		State state = State::Free;
		GpuDuration duration;
	};

	std::array<Slot, SlotCount> m_slots;
	std::size_t m_open = SlotCount;
};
}
