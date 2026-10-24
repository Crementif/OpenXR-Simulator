#pragma once

#include "graphics/FramePool.h"
#include "presentation/FrameHistory.h"
#include <array>
#include <memory>
#include <mutex>
#include <vector>

namespace simxr
{
class FrameOutput;
struct FramePreview
{
	FrameKey key;
	std::shared_ptr<const FrameOutput> output;
	bool submitted = false, clear = false;
};
// Independent of presentation ticks and debugger captures. A reservation stays
// charged until the last native owner retires, including readers after eviction.
class FramePreviewHistory
{
  public:
	static constexpr std::uint32_t Extent = 640;
	static constexpr std::uint64_t SlotBytes = 4ull << 20;
	static constexpr std::uint64_t BudgetBytes = 480ull << 20;
	static constexpr std::size_t Capacity = BudgetBytes / SlotBytes;
	~FramePreviewHistory();
	std::shared_ptr<FrameReservation> Reserve();
	const FramePool& Budget() const noexcept
	{
		return m_budget;
	}
	void Append(FrameKey key, std::shared_ptr<const FrameOutput> output, bool clear) noexcept;
	FramePreview Find(FrameKey key) const;
	std::vector<FrameKey> Frames(SessionGeneration generation) const;
	std::uint64_t Bytes() const
	{
		return m_budget.InUse() * SlotBytes;
	}

  private:
	FramePool m_budget{Capacity};
	mutable std::mutex m_mutex;
	std::array<FramePreview, Capacity> m_frames;
	std::size_t m_next = 0;
};
} // namespace simxr
