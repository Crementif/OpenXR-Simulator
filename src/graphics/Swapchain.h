#pragma once

#include <cstdint>
#include <deque>
#include <functional>
#include <optional>
#include <openxr/openxr.h>
#include <vector>

namespace simxr
{
class Swapchain
{
  public:
	explicit Swapchain(std::uint32_t imageCount, bool staticImage = false);
	XrResult Acquire(std::uint32_t& index, const std::function<XrResult(std::uint32_t)>& prepare = {});
	XrResult Wait(XrDuration timeout, bool ready);
	XrResult Release(std::uint32_t& index);
	std::optional<std::uint32_t> PendingImage() const;
	std::optional<std::uint32_t> ReleasedImage() const;

  private:
	enum class ImageState
	{
		Available,
		Acquired,
		Waited
	};
	std::vector<ImageState> m_images;
	std::deque<std::uint32_t> m_acquired;
	std::uint32_t m_next = 0;
	std::optional<std::uint32_t> m_released;
	bool m_staticImage;
	bool m_staticAcquired = false;
};
} // namespace simxr
