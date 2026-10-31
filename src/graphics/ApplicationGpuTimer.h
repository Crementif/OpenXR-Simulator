#pragma once

#include "graphics/FrameGpuTiming.h"
#include <array>
#include <cstddef>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <thread>

namespace simxr
{
// Frame markers require the native adapter's submission gate. Collection is
// serialized internally; asynchronous readers must be safe alongside application rendering.
class ApplicationGpuTimer
{
public:
	virtual ~ApplicationGpuTimer();
	bool StartCollecting() noexcept;
	// Derived destructors must stop collection before releasing native resources.
	void StopCollecting() noexcept;
	void Begin() noexcept;
	// Repeated submission attempts retain the same result until the next Begin.
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

	GpuDuration EndLocked() noexcept;
	void CollectLocked() noexcept;
	bool Pending() const noexcept;

	std::mutex m_mutex;
	std::condition_variable_any m_changed;
	std::array<Slot, SlotCount> m_slots;
	std::size_t m_open = SlotCount;
	GpuDuration m_ended;
	std::jthread m_collector;
};
}
