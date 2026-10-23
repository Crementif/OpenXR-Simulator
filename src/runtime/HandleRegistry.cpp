#include "runtime/HandleRegistry.h"
#include "Pch.h"

#include <atomic>
#include <limits>

namespace simxr
{
static std::atomic<std::uintptr_t> s_nextHandle{1};

std::uintptr_t AllocateHandleIdentity()
{
	auto identity = s_nextHandle.load(std::memory_order_relaxed);
	for (;;)
	{
		if (identity == std::numeric_limits<std::uintptr_t>::max())
			throw std::overflow_error("Handle identities exhausted");
		if (s_nextHandle.compare_exchange_weak(identity, identity + 1, std::memory_order_relaxed))
			return identity;
	}
}
} // namespace simxr
