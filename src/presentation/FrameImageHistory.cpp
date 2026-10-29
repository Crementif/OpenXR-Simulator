#include "presentation/FrameImageHistory.h"
#include "Pch.h"

namespace simxr
{
static HistoricalImage SelectImage(const HistoricalImage& image, ImageHistoryView view)
{
	return {image.metadata,
			view == ImageHistoryView::Quads ? image.quadOutput : image.output,
			{},
			image.quadBounds,
			image.quadVisible,
			view};
}

FrameImageWindow::~FrameImageWindow()
{
	std::array<HistoricalImage, Capacity> retired;
	std::lock_guard lock(m_mutex);
	retired.swap(m_entries);
	m_preCount = m_postCount = 0;
}

ImageWindowSnapshot FrameImageWindow::Snapshot() const
{
	ImageWindowSnapshot result;
	result.entries.reserve(Capacity);
	std::lock_guard lock(m_mutex);
	result.generation = m_generation;
	result.trigger = m_trigger;
	result.preCount = m_preCount;
	result.postCount = m_postCount;
	result.interrupted = m_interrupted;
	result.view = m_view;
	result.entries.insert(result.entries.end(), m_entries.begin(), m_entries.begin() + m_preCount + m_postCount);
	return result;
}

HistoricalImage FrameImageWindow::Find(FrameId frame) const
{
	std::lock_guard lock(m_mutex);
	for (std::size_t i = 0; i < m_preCount + m_postCount; ++i)
		if (m_entries[i].metadata->key.frame == frame)
			return m_entries[i];
	return {};
}

void FrameImageWindow::Cancel() noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_postCount != PostCount)
		m_interrupted = true;
}

void FrameImageWindow::Append(const HistoricalImage& image) noexcept
{
	std::lock_guard lock(m_mutex);
	if (m_interrupted || m_postCount == PostCount)
		return;
	m_entries[m_preCount + m_postCount++] = SelectImage(image, m_view);
}

FrameImageHistory::StartedWindow FrameImageHistory::BeginWindow(ImageHistoryView view)
{
	StartedWindow started{std::make_shared<FrameImageWindow>(), {}};
	std::lock_guard lock(m_mutex);
	started.previous = m_window.lock();
	if (started.previous)
	{
		std::lock_guard previousLock(started.previous->m_mutex);
		if (!started.previous->m_interrupted && started.previous->m_postCount != FrameImageWindow::PostCount)
		{
			started.window.reset();
			return started;
		}
	}
	if (!m_count)
	{
		started.window.reset();
		return started;
	}
	auto& window = *started.window;
	window.m_generation = m_generation;
	window.m_view = view;
	window.m_preCount = m_count;
	for (std::size_t i = 0; i < m_count; ++i)
		window.m_entries[i] = SelectImage(m_entries[(m_next + Capacity - m_count + i) % Capacity], view);
	window.m_trigger = window.m_entries[m_count - 1].metadata->key.frame;
	m_window = started.window;
	return started;
}

FrameImageHistory::~FrameImageHistory()
{
	auto retired = Clear();
}

FrameImageHistory::Retired FrameImageHistory::Start(SessionGeneration generation) noexcept
{
	Retired retired;
	std::lock_guard lock(m_mutex);
	retired.images.swap(m_entries);
	retired.window = m_window.lock();
	m_window.reset();
	if (retired.window)
		retired.window->Cancel();
	m_generation = generation;
	m_next = m_count = 0;
	m_evicted = m_unavailable = m_quadUnavailable = 0;
	return retired;
}

FrameImageHistory::Retired FrameImageHistory::Clear() noexcept
{
	Retired retired;
	std::lock_guard lock(m_mutex);
	retired.images.swap(m_entries);
	retired.window = m_window.lock();
	m_window.reset();
	if (retired.window)
		retired.window->Cancel();
	m_evicted += m_count;
	m_next = m_count = 0;
	return retired;
}

FrameImageHistory::Retired FrameImageHistory::Append(HistoricalImage image) noexcept
{
	Retired retired;
	std::lock_guard lock(m_mutex);
	if (!image.metadata || image.metadata->key.generation != m_generation)
	{
		retired.images[0] = std::move(image);
		return retired;
	}
	retired.window = m_window.lock();
	if (retired.window)
		retired.window->Append(image);
	if (!image.output && image.metadata->submission && image.metadata->submission->layerCount)
		++m_unavailable;
	if (image.quadVisible && !image.quadOutput)
		++m_quadUnavailable;
	retired.images[0] = std::exchange(m_entries[m_next], std::move(image));
	m_next = (m_next + 1) % Capacity;
	if (m_count == Capacity)
		++m_evicted;
	else
		++m_count;
	return retired;
}

ImageHistorySnapshot FrameImageHistory::Snapshot(std::size_t count, ImageHistoryView view) const
{
	if (!count || count > Capacity)
		throw std::invalid_argument("Image history count must be in [1,10]");
	ImageHistorySnapshot snapshot;
	snapshot.entries.reserve(count);
	std::lock_guard lock(m_mutex);
	snapshot.generation = m_generation;
	snapshot.evicted = m_evicted;
	snapshot.unavailable = view == ImageHistoryView::Quads ? m_quadUnavailable : m_unavailable;
	snapshot.view = view;
	count = std::min(count, m_count);
	for (std::size_t offset = 0; offset < count; ++offset)
		snapshot.entries.push_back(SelectImage(m_entries[(m_next + Capacity - count + offset) % Capacity], view));
	return snapshot;
}

HistoricalImage FrameImageHistory::Find(FrameKey frame, ImageHistoryView view) const
{
	std::lock_guard lock(m_mutex);
	if (frame.generation == m_generation)
		for (const auto& entry : m_entries)
			if (entry.metadata && entry.metadata->key == frame)
				return SelectImage(entry, view);
	return {};
}
} // namespace simxr
