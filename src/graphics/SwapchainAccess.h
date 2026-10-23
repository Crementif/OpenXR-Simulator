#pragma once

#include "graphics/GpuCompletion.h"
#include "graphics/Swapchain.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace simxr
{
class SwapchainAccess
{
  public:
	using InitializeImage = std::function<std::shared_ptr<const GpuCompletion>(std::uint32_t)>;

	explicit SwapchainAccess(std::uint32_t imageCount, bool staticImage = false);
	XrResult Acquire(std::uint32_t& index, const InitializeImage& initialize = {});
	XrResult Wait(XrDuration timeout);
	XrResult Release();
	std::optional<std::uint32_t> ReleasedImage() const;
	void MarkUsed(std::uint32_t index, const std::shared_ptr<const GpuCompletion>& completion);
	void MarkReleasedUsed(const std::shared_ptr<const GpuCompletion>& completion);
	void Close();

  private:
	XrResult WaitForImage(std::uint32_t index, XrDuration timeout);
	void FinishWait();

	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	Swapchain m_state;
	std::vector<std::weak_ptr<const GpuCompletion>> m_lastUse;
	bool m_closed = false;
	bool m_waiting = false;
};
} // namespace simxr
