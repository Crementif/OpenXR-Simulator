#pragma once

#include "app/ShellState.h"
#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>
#include <vector>

namespace simxr::app
{
enum HistoryFlags : std::uint32_t
{
	HistoryLate = 1 << 0,
	HistoryDropped = 1 << 1,
	HistorySnapshot = 1 << 2,
	HistoryPreview = 1 << 3,
	HistoryRecording = 1 << 4,
	HistoryObserved = 1 << 5,
	HistoryRunStart = 1 << 6,
	HistoryGap = 1 << 7,
	HistoryMarker = 1 << 8,
	HistoryPinned = 1 << 9,
	// A script, MCP or replay owned the head or a hand on this frame.
	HistoryDriven = 1 << 10,
	// At least one app action was pressed or moved.
	HistoryAction = 1 << 11
};
struct HistoryFrame
{
	FrameIdentity identity;
	std::uint64_t epoch = 0, capture = 0;
	// Monotonic canvas coordinate. The caller offsets each run without joining
	// frame numbers or pretending that disconnected time was recorded.
	double seconds = 0;
	// Interval between predicted display times, not GPU render duration.
	float frameMilliseconds = 0;
	std::uint32_t flags = 0;
	double runSeconds = 0;
	// InputHistory sequence, 0 when the frame carried no applied input.
	std::uint64_t input = 0;
	// Left/right press intensity, 0..255.
	std::array<std::uint8_t, 2> hands{};
};
struct HistoryBucket
{
	std::size_t count = 0;
	float minimum = 0, maximum = 0;
	std::uint32_t flags = 0;
	std::array<std::uint8_t, 2> hands{};
};
// Fixed-size frame ring with a hierarchy of min/max/flags buckets. Appending
// and querying visit O(log capacity) buckets, without per-frame allocation.
class HistoryStore
{
  public:
	explicit HistoryStore(std::size_t capacity = 162000);
	std::size_t Size() const noexcept
	{
		return m_size;
	}
	std::size_t Capacity() const noexcept
	{
		return m_frames.size();
	}
	void Clear() noexcept;
	void Append(const HistoryFrame& frame);
	const HistoryFrame& At(std::size_t index) const;
	// Coverage changes preserve the original frame/time identity.
	void SetCoverage(std::size_t index, std::uint64_t capture, std::uint32_t flags);
	void SetFlags(std::size_t index, std::uint32_t mask, bool enabled);
	std::optional<std::size_t> Find(FrameIdentity identity, std::uint64_t epoch) const;
	std::optional<std::size_t> Latest(FrameIdentity scope, std::uint64_t epoch) const;
	std::size_t LowerBound(double seconds) const noexcept;
	std::optional<std::size_t> Nearest(double seconds) const noexcept;
	HistoryBucket Query(std::size_t first, std::size_t end) const;

  private:
	// Consecutive frames of one run, as absolute append positions [begin, end).
	// Frame ids rise within a segment, so lookups binary-search one segment.
	struct Segment
	{
		std::uint64_t epoch = 0, session = 0, run = 0;
		std::size_t begin = 0, end = 0;
	};
	std::vector<Segment> m_segments;
	std::size_t m_appended = 0;
	std::vector<HistoryFrame> m_frames;
	std::vector<HistoryBucket> m_buckets;
	std::size_t m_start = 0, m_size = 0, m_leaves = 1;
	std::size_t Physical(std::size_t index) const noexcept;
	void Update(std::size_t physical) noexcept;
	HistoryBucket PhysicalQuery(std::size_t first, std::size_t end) const noexcept;
};
} // namespace simxr::app
