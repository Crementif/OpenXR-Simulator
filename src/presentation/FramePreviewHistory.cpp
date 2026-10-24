#include "presentation/FramePreviewHistory.h"
#include "Pch.h"
#include "graphics/FrameOutput.h"

namespace simxr
{
FramePreviewHistory::~FramePreviewHistory()
{
	std::array<FramePreview, Capacity> retired;
	std::lock_guard lock(m_mutex);
	retired.swap(m_frames);
}
std::shared_ptr<FrameReservation> FramePreviewHistory::Reserve()
{
	if (auto reservation = m_budget.Reserve())
		return reservation;
	for (std::size_t offset = 0; offset < Capacity; ++offset)
	{
		std::shared_ptr<const FrameOutput> retired;
		{
			std::lock_guard lock(m_mutex);
			retired = std::move(m_frames[(m_next + offset) % Capacity].output);
		}
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
	m_next = (m_next + 1) % Capacity;
}
std::vector<FrameKey> FramePreviewHistory::Frames(SessionGeneration generation) const
{
	std::vector<FrameKey> frames;
	frames.reserve(Capacity);
	std::lock_guard lock(m_mutex);
	for (std::size_t offset = 0; offset < Capacity; ++offset)
	{
		const auto& frame = m_frames[(m_next + offset) % Capacity];
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
