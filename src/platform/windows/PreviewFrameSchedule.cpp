#include "platform/windows/PreviewFrameSchedule.h"

#include <limits>

namespace simxr::windows
{
std::optional<PreviewExtent> PreviewExtent::Make(std::uint32_t width, std::uint32_t height)
{
	if (width == 0 || height == 0 || width > 16384 || height > 16384)
		return {};
	const auto pitch = (static_cast<std::uint64_t>(width) * 4 + 255) & ~std::uint64_t{255};
	return PreviewExtent{width, height, static_cast<std::uint32_t>(pitch), pitch * height};
}

std::optional<std::size_t> PreviewFrameSchedule::Begin()
{
	if (m_closed)
		return {};
	if (m_recording)
		return m_recording;
	for (std::size_t index = 0; index < Capacity; ++index)
		if (m_slots[index].phase == Phase::Free)
		{
			m_slots[index].phase = Phase::Recording;
			m_recording = index;
			return index;
		}
	return {};
}

std::optional<std::uint64_t> PreviewFrameSchedule::Queue()
{
	if (!m_recording || m_lastQueued == std::numeric_limits<std::uint64_t>::max() - 1)
		return {};
	auto& slot = m_slots[*m_recording];
	slot.phase = Phase::Queued;
	slot.fence = ++m_lastQueued;
	m_recording.reset();
	return slot.fence;
}

bool PreviewFrameSchedule::Submitted(std::size_t index)
{
	if (index >= Capacity || m_slots[index].phase != Phase::Queued)
		return false;
	m_slots[index].phase = Phase::Submitted;
	return true;
}

void PreviewFrameSchedule::Observe(std::uint64_t completedFence)
{
	// D3D12 uses UINT64_MAX to report device removal, not completion.
	if (completedFence == std::numeric_limits<std::uint64_t>::max())
		return;
	for (auto& slot : m_slots)
		if (slot.phase == Phase::Submitted && slot.fence <= completedFence)
			slot.phase = Phase::Complete;
}

bool PreviewFrameSchedule::Release(std::size_t index)
{
	if (index >= Capacity || m_slots[index].phase != Phase::Complete)
		return false;
	m_slots[index].phase = Phase::Free;
	return true;
}

void PreviewFrameSchedule::CancelRecording()
{
	if (m_recording)
		m_slots[*m_recording].phase = Phase::Free;
	m_recording.reset();
}

void PreviewFrameSchedule::CloseAdmission()
{
	m_closed = true;
	CancelRecording();
}

std::optional<std::size_t> PreviewFrameSchedule::Recording() const
{
	return m_recording;
}
PreviewFrameSchedule::Phase PreviewFrameSchedule::State(std::size_t index) const
{
	return index < Capacity ? m_slots[index].phase : Phase::Free;
}
std::uint64_t PreviewFrameSchedule::Fence(std::size_t index) const
{
	return index < Capacity ? m_slots[index].fence : 0;
}
std::uint64_t PreviewFrameSchedule::LastQueued() const
{
	return m_lastQueued;
}
bool PreviewFrameSchedule::Idle() const
{
	for (const auto& slot : m_slots)
		if (slot.phase == Phase::Queued || slot.phase == Phase::Submitted)
			return false;
	return true;
}
} // namespace simxr::windows
