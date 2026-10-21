#pragma once

#include <array>
#include <cstddef>
#include <cstdint>
#include <optional>

namespace simxr::windows
{
struct PreviewExtent
{
	std::uint32_t width = 0;
	std::uint32_t height = 0;
	std::uint32_t rowPitch = 0;
	std::uint64_t readbackBytes = 0;
	static std::optional<PreviewExtent> Make(std::uint32_t width, std::uint32_t height);
};

// Host publication and actual fence observation are separate transitions. Neither
// queue admission nor a DXGI Present return makes a frame reusable.
class PreviewFrameSchedule
{
  public:
	static constexpr std::size_t Capacity = 3;
	enum class Phase
	{
		Free,
		Recording,
		Queued,
		Submitted,
		Complete
	};
	std::optional<std::size_t> Begin();
	std::optional<std::uint64_t> Queue();
	bool Submitted(std::size_t index);
	void Observe(std::uint64_t completedFence);
	bool Release(std::size_t index);
	void CancelRecording();
	void CloseAdmission();
	std::optional<std::size_t> Recording() const;
	Phase State(std::size_t index) const;
	std::uint64_t Fence(std::size_t index) const;
	std::uint64_t LastQueued() const;
	bool Idle() const;

  private:
	struct Slot
	{
		Phase phase = Phase::Free;
		std::uint64_t fence = 0;
	};
	std::array<Slot, Capacity> m_slots{};
	std::optional<std::size_t> m_recording;
	std::uint64_t m_lastQueued = 0;
	bool m_closed = false;
};
} // namespace simxr::windows
