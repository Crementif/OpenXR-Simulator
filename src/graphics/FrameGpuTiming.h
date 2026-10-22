#pragma once

#include <atomic>
#include <cstdint>

namespace simxr
{
// Numeric observation only: history must never retain a GPU submission or device.
struct FrameGpuTiming
{
	// -1 means unavailable/pending. This measures runtime composition, not app rendering.
	std::atomic<std::int64_t> compositorNanoseconds{-1};
};
} // namespace simxr
