#include "presentation/FrameImageHistory.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace simxr
{
FrameImageHistory::~FrameImageHistory()
{
	auto retired = Clear();
}
FrameImageHistory::Retired FrameImageHistory::Start(SessionGeneration generation) noexcept
{
	Retired retired;
	std::lock_guard lock(m_mutex);
	retired.swap(m_entries);
	m_generation = generation;
	m_next = m_count = 0;
	m_evicted = m_unavailable = 0;
	return retired;
}
FrameImageHistory::Retired FrameImageHistory::Clear() noexcept
{
	Retired retired;
	std::lock_guard lock(m_mutex);
	retired.swap(m_entries);
	m_evicted += m_count;
	m_next = m_count = 0;
	return retired;
}
HistoricalImage FrameImageHistory::Append(HistoricalImage image) noexcept
{
	std::lock_guard lock(m_mutex);
	if (!image.metadata || image.metadata->key.generation != m_generation)
		return image;
	if (!image.output && image.metadata->submission && image.metadata->submission->layerCount)
		++m_unavailable;
	auto retired = std::exchange(m_entries[m_next], std::move(image));
	m_next = (m_next + 1) % Capacity;
	if (m_count == Capacity)
		++m_evicted;
	else
		++m_count;
	return retired;
}
ImageHistorySnapshot FrameImageHistory::Snapshot(std::size_t count) const
{
	if (!count || count > Capacity)
		throw std::invalid_argument("Image history count must be in [1,10]");
	ImageHistorySnapshot snapshot;
	snapshot.entries.reserve(count);
	std::lock_guard lock(m_mutex);
	snapshot.generation = m_generation;
	snapshot.evicted = m_evicted;
	snapshot.unavailable = m_unavailable;
	count = std::min(count, m_count);
	for (std::size_t offset = 0; offset < count; ++offset)
		snapshot.entries.push_back(m_entries[(m_next + Capacity - count + offset) % Capacity]);
	return snapshot;
}
HistoricalImage FrameImageHistory::Find(FrameKey frame) const
{
	std::lock_guard lock(m_mutex);
	if (frame.generation == m_generation)
		for (const auto& entry : m_entries)
			if (entry.metadata && entry.metadata->key == frame)
				return entry;
	return {};
}
} // namespace simxr
