#include "graphics/SnapshotBudget.h"
#include "Pch.h"

namespace simxr
{
static std::atomic<std::uint32_t> s_budgetMiB{SnapshotBudget::DefaultMiB};
static std::atomic<std::uint64_t> s_imageBytes{16 * 1024 * 1024};
void SnapshotBudget::Configure(std::uint32_t mib)
{
	if (mib < MinMiB || mib > MaxMiB)
		throw std::invalid_argument("Snapshot budget is outside its supported range");
	s_budgetMiB.store(mib);
}
void SnapshotBudget::ObserveImageBytes(std::uint64_t bytes) noexcept
{
	auto previous = s_imageBytes.load();
	while (previous < bytes && !s_imageBytes.compare_exchange_weak(previous, bytes))
	{
	}
}
std::uint32_t SnapshotBudget::MiB() noexcept
{
	return s_budgetMiB.load();
}
std::uint64_t SnapshotBudget::ImageBytes() noexcept
{
	return s_imageBytes.load();
}
std::size_t SnapshotBudget::Capacity() noexcept
{
	return CapacityFor(MiB(), ImageBytes());
}
std::size_t SnapshotBudget::CapacityFor(std::uint32_t mib, std::uint64_t imageBytes) noexcept
{
	return std::min<std::uint64_t>(MaxCapacity, std::uint64_t{mib} * 1024 * 1024 /
										 std::max<std::uint64_t>(imageBytes, 16 * 1024 * 1024) / 4);
}
} // namespace simxr
