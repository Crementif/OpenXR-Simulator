#include "graphics/Swapchain.h"

#include <stdexcept>

namespace simxr
{
Swapchain::Swapchain(std::uint32_t imageCount, bool staticImage) :
	m_images(imageCount, ImageState::Available), m_staticImage(staticImage)
{
	if (imageCount == 0 || (staticImage && imageCount != 1))
		throw std::invalid_argument("Invalid swapchain image count");
}

XrResult Swapchain::Acquire(std::uint32_t& index, const std::function<XrResult(std::uint32_t)>& prepare)
{
	if (m_staticImage && m_staticAcquired)
		return XR_ERROR_CALL_ORDER_INVALID;
	for (std::uint32_t offset = 0; offset < m_images.size(); ++offset)
	{
		auto candidate = (m_next + offset) % m_images.size();
		if (m_images[candidate] != ImageState::Available)
			continue;
		m_acquired.push_back(static_cast<std::uint32_t>(candidate));
		try
		{
			if (prepare)
			{
				const auto result = prepare(static_cast<std::uint32_t>(candidate));
				if (XR_FAILED(result))
				{
					m_acquired.pop_back();
					return result;
				}
			}
		}
		catch (...)
		{
			m_acquired.pop_back();
			throw;
		}
		m_images[candidate] = ImageState::Acquired;
		m_next = static_cast<std::uint32_t>((candidate + 1) % m_images.size());
		m_staticAcquired = true;
		index = static_cast<std::uint32_t>(candidate);
		return XR_SUCCESS;
	}
	return XR_ERROR_CALL_ORDER_INVALID;
}

XrResult Swapchain::Wait(XrDuration timeout, bool ready)
{
	if (timeout < 0)
		return XR_ERROR_VALIDATION_FAILURE;
	if (m_acquired.empty() || m_images[m_acquired.front()] != ImageState::Acquired)
		return XR_ERROR_CALL_ORDER_INVALID;
	if (!ready)
		return XR_TIMEOUT_EXPIRED;
	m_images[m_acquired.front()] = ImageState::Waited;
	return XR_SUCCESS;
}

XrResult Swapchain::Release(std::uint32_t& index)
{
	if (m_acquired.empty() || m_images[m_acquired.front()] != ImageState::Waited)
		return XR_ERROR_CALL_ORDER_INVALID;
	index = m_acquired.front();
	m_images[index] = ImageState::Available;
	m_released = index;
	m_acquired.pop_front();
	return XR_SUCCESS;
}

std::optional<std::uint32_t> Swapchain::PendingImage() const
{
	if (m_acquired.empty())
		return {};
	return m_acquired.front();
}

std::optional<std::uint32_t> Swapchain::ReleasedImage() const
{
	return m_released;
}
}
