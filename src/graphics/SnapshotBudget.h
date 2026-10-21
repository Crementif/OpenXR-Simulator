#pragma once

#include <cstddef>
#include <cstdint>

namespace simxr
{
// All exact-snapshot owners use this process-wide policy. Four image allocations
// cover production, owned inspection, delivery and the companion-private copy.
class SnapshotBudget
{
  public:
	static constexpr std::uint32_t DefaultMiB = 1024;
	static constexpr std::uint32_t MinMiB = 128;
	static constexpr std::uint32_t MaxMiB = 8192;
	static constexpr std::size_t MaxCapacity = 128;
	static void Configure(std::uint32_t mib);
	static void ObserveImageBytes(std::uint64_t bytes) noexcept;
	static std::uint32_t MiB() noexcept;
	static std::uint64_t ImageBytes() noexcept;
	static std::size_t Capacity() noexcept;
};
} // namespace simxr
