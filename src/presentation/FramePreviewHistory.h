#pragma once
#include "graphics/ArchivedFrame.h"
#include "graphics/FramePool.h"
#include "presentation/FrameTypes.h"
#include "simulation/TrackingTypes.h"
#include <atomic>
#include <condition_variable>
#include <deque>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <vector>

namespace simxr
{
class FrameOutput;
struct FramePreview
{
	FrameKey key;
	std::shared_ptr<const FrameOutput> output;
	bool submitted = false, clear = false, pending = false;
	std::shared_ptr<const TrackingSnapshot> tracking;
};
struct PreviewClipFrame
{
	FrameKey key;
	RuntimeTime time;
	Duration period;
	std::shared_ptr<const FrameOutput> output;
	std::shared_ptr<const ArchivedFrame> archive;
	std::shared_ptr<const void> retention;
	bool clear = false;
};
class FramePreviewHistory
{
  public:
	static constexpr std::uint32_t Extent = 640;
	static constexpr std::uint64_t SlotBytes = 4ull << 20, BudgetBytes = 480ull << 20, RamBudgetBytes = 1024ull << 20;
	static constexpr std::size_t Capacity = BudgetBytes / SlotBytes;
	static constexpr std::uint32_t MinMiB = 64, MaxMiB = 16384;
	FramePreviewHistory();
	~FramePreviewHistory();
	void Configure(std::uint64_t bytes);
	void CloseAdmission();
	void ConfigureRam(std::uint64_t bytes);
	std::uint64_t Limit() const
	{
		return m_budget.Capacity() * SlotBytes;
	}
	std::uint64_t Bytes() const
	{
		return m_budget.InUse() * SlotBytes;
	}
	std::uint64_t RamLimit() const
	{
		return m_ramLimit.load();
	}
	std::uint64_t RamBytes() const
	{
		return m_ramBytes->load();
	}
	std::string Error() const;
	std::shared_ptr<FrameReservation> Reserve(FrameKey key, bool waitForSpace = true);
	const FramePool& Budget() const noexcept
	{
		return m_budget;
	}
	void Append(FrameKey key, std::shared_ptr<const FrameOutput> output, bool clear,
				const TrackingSnapshot* tracking = nullptr, RuntimeTime time = {}, Duration period = {});
	FramePreview Find(FrameKey key);
	std::vector<PreviewClipFrame> RetainRange(FrameKey first, FrameKey last) const;
	std::shared_ptr<const TrackingSnapshot> Tracking(FrameKey key) const;
	std::vector<FrameKey> Frames(SessionGeneration generation, std::size_t limit = SIZE_MAX) const;

  private:
	struct Entry
	{
		FramePreview preview;
		RuntimeTime time;
		Duration period;
		TrackingSnapshot tracking;
		bool hasTracking = false;
		Entry();
		std::uint64_t StateBytes() const noexcept;
		std::weak_ptr<FrameReservation> admission;
		std::shared_ptr<const ArchivedFrame> archive;
		std::shared_ptr<std::atomic<std::uint64_t>> counter;
		std::uint64_t charge = 0;
		~Entry()
		{
			if (counter)
				counter->fetch_sub(charge);
		}
	};
	std::shared_ptr<Entry> EvictRaw();
	void TrimRam();
	void Run() noexcept;
	FramePool m_budget{Capacity};
	std::shared_ptr<std::atomic<std::uint64_t>> m_ramBytes = std::make_shared<std::atomic<std::uint64_t>>(0);
	std::atomic<std::uint64_t> m_ramLimit{RamBudgetBytes};
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	std::map<FrameKey, std::shared_ptr<Entry>> m_frames;
	// Bounded by GPU admission; archived entries never participate in worker scans.
	std::deque<std::shared_ptr<Entry>> m_recent;
	FrameKey m_requested{};
	FramePreview m_decoded;
	std::string m_error;
	bool m_admissionClosed = false;
	bool m_stopping = false, m_gpuOnly = false, m_trimming = false;
	std::thread m_worker;
};
} // namespace simxr
