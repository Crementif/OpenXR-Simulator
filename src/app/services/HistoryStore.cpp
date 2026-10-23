#include "app/services/HistoryStore.h"
#include "Pch.h"
#include <cmath>
#include <limits>
#include <stdexcept>

namespace simxr::app
{
static HistoryBucket Merge(HistoryBucket a, HistoryBucket b) noexcept
{
	if (!a.count)
		return b;
	if (!b.count)
		return a;
	return {a.count + b.count,
			std::min(a.minimum, b.minimum),
			std::max(a.maximum, b.maximum),
			a.flags | b.flags,
			{std::max(a.hands[0], b.hands[0]), std::max(a.hands[1], b.hands[1])},
			std::max(a.cpuMaximum, b.cpuMaximum),
			a.missed + b.missed,
			a.driven + b.driven};
}
HistoryStore::HistoryStore(std::size_t capacity)
{
	if (!capacity || capacity > 1000000)
		throw std::invalid_argument("Invalid frame history capacity");
	m_frames.resize(capacity);
	while (m_leaves < capacity)
		m_leaves *= 2;
	m_buckets.resize(m_leaves * 2);
}
void HistoryStore::Clear() noexcept
{
	m_start = m_size = 0;
	m_appended = 0;
	m_segments.clear();
	std::fill(m_buckets.begin(), m_buckets.end(), HistoryBucket{});
}
std::size_t HistoryStore::Physical(std::size_t index) const noexcept
{
	return (m_start + index) % Capacity();
}
const HistoryFrame& HistoryStore::At(std::size_t index) const
{
	if (index >= m_size)
		throw std::out_of_range("Frame history index");
	return m_frames[Physical(index)];
}
void HistoryStore::Update(std::size_t physical) noexcept
{
	const auto& frame = m_frames[physical];
	auto node = m_leaves + physical;
	m_buckets[node] = {1,
					   frame.frameMilliseconds,
					   frame.frameMilliseconds,
					   frame.flags,
					   frame.hands,
					   frame.cpuMilliseconds,
					   (frame.flags & (HistoryLate | HistoryDropped)) ? 1U : 0U,
					   (frame.flags & HistoryDriven) ? 1U : 0U};
	while (node > 1)
	{
		node /= 2;
		m_buckets[node] = Merge(m_buckets[node * 2], m_buckets[node * 2 + 1]);
	}
}
void HistoryStore::Append(const HistoryFrame& frame)
{
	if (!std::isfinite(frame.seconds) || !std::isfinite(frame.frameMilliseconds) || frame.frameMilliseconds < 0 ||
		(m_size && frame.seconds < At(m_size - 1).seconds))
		throw std::invalid_argument("Frame history requires finite ordered times");
	if (m_appended == std::numeric_limits<std::uint64_t>::max())
		throw std::overflow_error("Frame history sequence exhausted");
	const auto physical = Physical(m_size);
	const auto& run = frame.identity;
	if (m_segments.empty() || m_segments.back().end != m_appended || m_segments.back().epoch != frame.epoch ||
		m_segments.back().session != run.session || m_segments.back().run != run.run ||
		(frame.flags & HistoryRunStart) || run.frame <= At(m_size - 1).identity.frame)
		m_segments.push_back({frame.epoch, run.session, run.run, m_appended, m_appended});
	++m_segments.back().end;
	++m_appended;
	m_frames[physical] = frame;
	if (m_size == Capacity())
		m_start = (m_start + 1) % Capacity();
	else
		++m_size;
	const auto oldest = m_appended - m_size;
	if (m_segments.front().end <= oldest)
		m_segments.erase(m_segments.begin());
	Update(physical);
}
void HistoryStore::SetCoverage(std::size_t index, std::uint64_t capture, std::uint32_t flags)
{
	(void)At(index);
	const auto physical = Physical(index);
	auto& frame = m_frames[physical];
	frame.capture = capture;
	constexpr auto coverage = HistorySnapshot | HistoryPreview;
	frame.flags = (frame.flags & ~coverage) | (flags & coverage);
	Update(physical);
}
std::optional<std::size_t> HistoryStore::Find(FrameIdentity identity, std::uint64_t epoch) const
{
	const auto oldest = m_appended - m_size;
	for (auto segment = m_segments.rbegin(); segment != m_segments.rend(); ++segment)
	{
		if (segment->epoch != epoch || segment->session != identity.session || segment->run != identity.run)
			continue;
		auto first = static_cast<std::size_t>(std::max(segment->begin, oldest) - oldest);
		auto end = static_cast<std::size_t>(segment->end - oldest);
		while (first < end)
		{
			const auto middle = first + (end - first) / 2;
			const auto frame = m_frames[Physical(middle)].identity.frame;
			if (frame == identity.frame)
				return middle;
			if (frame < identity.frame)
				first = middle + 1;
			else
				end = middle;
		}
	}
	return {};
}
std::optional<std::size_t> HistoryStore::Latest(FrameIdentity scope, std::uint64_t epoch) const
{
	if (const auto segment = LatestSegment(scope, epoch))
		return segment->second - 1;
	return {};
}
std::optional<std::pair<std::size_t, std::size_t>> HistoryStore::LatestSegment(FrameIdentity scope,
																			   std::uint64_t epoch) const
{
	const auto oldest = m_appended - m_size;
	for (auto segment = m_segments.rbegin(); segment != m_segments.rend(); ++segment)
		if (segment->epoch == epoch && segment->session == scope.session && segment->run == scope.run)
			return std::pair{static_cast<std::size_t>(std::max(segment->begin, oldest) - oldest),
							 static_cast<std::size_t>(segment->end - oldest)};
	return {};
}
std::pair<std::size_t, std::size_t> HistoryStore::SegmentAt(std::size_t index) const
{
	if (index >= m_size)
		throw std::out_of_range("Frame history segment");
	const auto oldest = m_appended - m_size;
	const auto segment = std::lower_bound(m_segments.begin(), m_segments.end(), oldest + index,
										  [](const Segment& candidate, std::uint64_t position)
										  {
											  return candidate.end <= position;
										  });
	return {static_cast<std::size_t>(std::max(segment->begin, oldest) - oldest),
			static_cast<std::size_t>(segment->end - oldest)};
}
void HistoryStore::SetFlags(std::size_t index, std::uint32_t mask, bool enabled)
{
	(void)At(index);
	const auto physical = Physical(index);
	auto& flags = m_frames[physical].flags;
	flags = enabled ? flags | mask : flags & ~mask;
	Update(physical);
}
std::size_t HistoryStore::LowerBound(double seconds) const noexcept
{
	std::size_t first = 0, end = m_size;
	while (first < end)
	{
		const auto middle = first + (end - first) / 2;
		if (m_frames[Physical(middle)].seconds < seconds)
			first = middle + 1;
		else
			end = middle;
	}
	return first;
}
std::optional<std::size_t> HistoryStore::Nearest(double seconds) const noexcept
{
	if (!m_size || !std::isfinite(seconds))
		return {};
	const auto next = LowerBound(seconds);
	if (!next)
		return 0;
	if (next == m_size)
		return m_size - 1;
	return seconds - m_frames[Physical(next - 1)].seconds <= m_frames[Physical(next)].seconds - seconds ? next - 1
																										: next;
}
HistoryBucket HistoryStore::PhysicalQuery(std::size_t first, std::size_t end) const noexcept
{
	HistoryBucket result;
	for (first += m_leaves, end += m_leaves; first < end; first /= 2, end /= 2)
	{
		if (first & 1)
			result = Merge(result, m_buckets[first++]);
		if (end & 1)
			result = Merge(result, m_buckets[--end]);
	}
	return result;
}
HistoryBucket HistoryStore::Query(std::size_t first, std::size_t end) const
{
	if (first > end || end > m_size)
		throw std::out_of_range("Frame history range");
	if (first == end)
		return {};
	const auto start = Physical(first), count = end - first;
	const auto tail = std::min(count, Capacity() - start);
	return Merge(PhysicalQuery(start, start + tail), PhysicalQuery(0, count - tail));
}
} // namespace simxr::app
