#pragma once

#include <atomic>
#include <cstdint>
#include <memory>

namespace simxr
{
// Copies share the eventual numeric result without retaining native resources.
class GpuDuration
{
public:
	static GpuDuration Create()
	{
		GpuDuration result;
		result.m_value = std::make_shared<std::atomic<std::int64_t>>(-1);
		return result;
	}

	std::int64_t Load() const noexcept
	{
		return m_value ? m_value->load() : -1;
	}

	void Store(std::int64_t value) const noexcept
	{
		if (m_value)
			m_value->store(value);
	}

private:
	std::shared_ptr<std::atomic<std::int64_t>> m_value;
};

// Numeric observation only: history must never retain a GPU submission or device.
struct FrameGpuTiming
{
	// -1 means unavailable/pending. This measures runtime composition, not app rendering.
	GpuDuration compositorNanoseconds = GpuDuration::Create();
	// Queue interval between frame markers, including any queue idle time.
	GpuDuration applicationNanoseconds;
};
} // namespace simxr
