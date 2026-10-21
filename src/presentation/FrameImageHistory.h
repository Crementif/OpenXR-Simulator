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

struct ImageWindowSnapshot
{
	SessionGeneration generation;
	FrameId trigger;
	std::size_t preCount = 0;
	std::size_t postCount = 0;
	bool interrupted = false;
	std::vector<HistoricalImage> entries;
};
class FrameImageWindow
{
	friend class FrameImageHistory;
  public:
	static constexpr std::size_t PostCount = 12;
	static constexpr std::size_t Capacity = 22;
	~FrameImageWindow();
	ImageWindowSnapshot Snapshot() const;
	HistoricalImage Find(FrameId frame) const;
	void Cancel() noexcept;

  private:
	void Append(const HistoricalImage& image) noexcept;
	mutable std::mutex m_mutex;
	std::array<HistoricalImage, Capacity> m_entries;
	SessionGeneration m_generation;
	FrameId m_trigger;
	std::size_t m_preCount = 0, m_postCount = 0;
	bool m_interrupted = false;
};

// The caller promotes supported GPU outputs into Budget before publishing them.
// Returned retired readers must outlive caller locks that native destruction may reenter.
class FrameImageHistory
{
  public:
	static constexpr std::size_t Capacity = 10;
	static constexpr std::uint64_t MaxImageBytes = 16 * 1024 * 1024;
	struct Retired
	{
		std::array<HistoricalImage, Capacity> images;
		std::shared_ptr<FrameImageWindow> window;
	};
	struct StartedWindow
	{
		std::shared_ptr<FrameImageWindow> window;
		std::shared_ptr<FrameImageWindow> previous;
	};
	~FrameImageHistory();
	const FramePool& Budget() const noexcept { return m_budget; }
	Retired Start(SessionGeneration generation) noexcept;
	Retired Clear() noexcept;
	Retired Append(HistoricalImage image) noexcept;
	StartedWindow BeginWindow();
	ImageHistorySnapshot Snapshot(std::size_t count = Capacity) const;
	HistoricalImage Find(FrameKey frame) const;

  private:
	mutable std::mutex m_mutex;
	FramePool m_budget{24};
	std::array<HistoricalImage, Capacity> m_entries;
	std::weak_ptr<FrameImageWindow> m_window;
	SessionGeneration m_generation;
	std::size_t m_next = 0, m_count = 0;
	std::uint64_t m_evicted = 0, m_unavailable = 0;
};
} // namespace simxr
