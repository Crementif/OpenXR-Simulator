#include "platform/ProcessIdentity.h"
#include "Pch.h"

namespace simxr
{
static std::atomic<std::uint64_t> s_nextIdentity{1};

std::uint64_t AllocateProcessIdentity()
{
	auto identity = s_nextIdentity.load(std::memory_order_relaxed);
	for (;;)
	{
		if (identity == std::numeric_limits<std::uint64_t>::max())
			throw std::overflow_error("Process identities exhausted");
		if (s_nextIdentity.compare_exchange_weak(identity, identity + 1, std::memory_order_relaxed))
			return identity;
	}
}
} // namespace simxr
