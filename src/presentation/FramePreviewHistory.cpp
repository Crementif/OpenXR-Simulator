#include "presentation/FramePreviewHistory.h"
#include "Pch.h"
#include "graphics/FrameOutput.h"

namespace simxr
{
FramePreviewHistory::~FramePreviewHistory()
{
	std::vector<FramePreview> retired;
	std::lock_guard lock(m_mutex);
	retired.swap(m_frames);
}
void FramePreviewHistory::Configure(std::uint64_t bytes)
{
	if (bytes < (std::uint64_t{MinMiB} << 20) || bytes > (std::uint64_t{MaxMiB} << 20) || bytes % SlotBytes)
		throw std::invalid_argument("History budget must be 64–16384 MiB in multiples of 4 MiB");
	const auto capacity = static_cast<std::size_t>(bytes / SlotBytes);
	std::vector<FramePreview> resized(capacity);
	{
		std::lock_guard lock(m_mutex);
		const auto keep = std::min(capacity, m_frames.size());
		for (std::size_t offset = 0; offset < keep; ++offset)
			resized[offset] = std::move(m_frames[(m_next + m_frames.size() - keep + offset) % m_frames.size()]);
		m_next = keep % capacity;
		m_frames.swap(resized);
		m_budget.SetCapacity(capacity);
	}
	resized.clear();
	// Detached readers remain charged; shrinking never invalidates their GPU owners.
	while (m_budget.InUse() > capacity)
		if (!EvictOldest())
			break;
}
std::shared_ptr<const FrameOutput> FramePreviewHistory::EvictOldest()
{
	std::lock_guard lock(m_mutex);
	for (std::size_t offset = 0; offset < m_frames.size(); ++offset)
	{
		auto& frame = m_frames[(m_next + offset) % m_frames.size()];
		if (frame.output)
			return std::move(frame.output);
	}
	return {};
}
std::shared_ptr<FrameReservation> FramePreviewHistory::Reserve()
{
	if (auto reservation = m_budget.Reserve())
		return reservation;
	while (auto retired = EvictOldest())
	{
		retired.reset();
		if (auto reservation = m_budget.Reserve())
			return reservation;
	}
	return {};
}

void FramePreviewHistory::Append(FrameKey key, std::shared_ptr<const FrameOutput> output, bool clear) noexcept
{
	FramePreview retired;
	std::lock_guard lock(m_mutex);
	retired = std::exchange(m_frames[m_next], FramePreview{key, std::move(output), true, clear});
	m_next = (m_next + 1) % m_frames.size();
}
std::vector<FrameKey> FramePreviewHistory::Frames(SessionGeneration generation) const
{
	std::vector<FrameKey> frames;
	std::lock_guard lock(m_mutex);
	frames.reserve(m_frames.size());
	for (std::size_t offset = 0; offset < m_frames.size(); ++offset)
	{
		const auto& frame = m_frames[(m_next + offset) % m_frames.size()];
		if (frame.submitted && frame.key.generation == generation && frame.output)
			frames.push_back(frame.key);
	}
	return frames;
}
FramePreview FramePreviewHistory::Find(FrameKey key) const
{
	std::lock_guard lock(m_mutex);
	for (const auto& frame : m_frames)
		if (frame.submitted && frame.key == key)
			return frame;
	return {};
}
} // namespace simxr
