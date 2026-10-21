#pragma once

#include "graphics/FramePool.h"
#include "presentation/FrameHistory.h"

#include <array>
#include <mutex>

namespace simxr
{
class FrameOutput;
struct HistoricalImage
{
	std::shared_ptr<const FrameRecord> metadata;
	std::shared_ptr<const FrameOutput> output;
};
struct ImageHistorySnapshot
{
	SessionGeneration generation;
	std::uint64_t evicted = 0;
	std::uint64_t unavailable = 0;
	std::vector<HistoricalImage> entries;
};

// The caller promotes supported GPU outputs into Budget before publishing them.
// Returned retired readers must outlive caller locks that native destruction may reenter.
class FrameImageHistory
{
  public:
	static constexpr std::size_t Capacity = 10;
	static constexpr std::uint64_t MaxImageBytes = 16 * 1024 * 1024;
	using Retired = std::array<HistoricalImage, Capacity>;
	~FrameImageHistory();
	const FramePool& Budget() const noexcept { return m_budget; }
	Retired Start(SessionGeneration generation) noexcept;
	Retired Clear() noexcept;
	HistoricalImage Append(HistoricalImage image) noexcept;
	ImageHistorySnapshot Snapshot(std::size_t count = Capacity) const;
	HistoricalImage Find(FrameKey frame) const;

  private:
	mutable std::mutex m_mutex;
	FramePool m_budget{24};
	Retired m_entries;
	SessionGeneration m_generation;
	std::size_t m_next = 0, m_count = 0;
	std::uint64_t m_evicted = 0, m_unavailable = 0;
};
} // namespace simxr
