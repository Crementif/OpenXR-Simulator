#include "graphics/SwapchainAccess.h"
#include "Pch.h"

namespace simxr
{
SwapchainAccess::SwapchainAccess(std::uint32_t imageCount, bool staticImage)
	: m_state(imageCount, staticImage), m_lastUse(imageCount)
{}

XrResult SwapchainAccess::Acquire(std::uint32_t& index, const InitializeImage& initialize)
{
	std::shared_ptr<const GpuCompletion> initialized;
	std::lock_guard lock(m_mutex);
	if (m_closed)
		return XR_ERROR_SESSION_LOST;
	return m_state.Acquire(index, [&](std::uint32_t candidate) {
		if (initialize)
		{
			initialized = initialize(candidate);
			if (initialized)
				m_lastUse[candidate] = initialized;
		}
		return XR_SUCCESS;
	});
}

XrResult SwapchainAccess::Wait(XrDuration timeout)
{
	std::uint32_t index;
	{
		std::lock_guard lock(m_mutex);
		if (m_closed)
			return XR_ERROR_SESSION_LOST;
		if (m_waiting)
			return XR_ERROR_CALL_ORDER_INVALID;
		const auto validation = m_state.Wait(timeout, false);
		if (validation != XR_TIMEOUT_EXPIRED)
			return validation;
		index = *m_state.PendingImage();
		m_waiting = true;
	}

	struct WaitRegistration
	{
		SwapchainAccess& owner;

		~WaitRegistration()
		{
			owner.FinishWait();
		}
	} registration{*this};

	return WaitForImage(index, timeout);
}

XrResult SwapchainAccess::Release()
{
	std::lock_guard lock(m_mutex);
	if (m_closed)
		return XR_ERROR_SESSION_LOST;
	if (m_waiting)
		return XR_ERROR_CALL_ORDER_INVALID;
	std::uint32_t index;
	return m_state.Release(index);
}

std::optional<std::uint32_t> SwapchainAccess::ReleasedImage() const
{
	std::lock_guard lock(m_mutex);
	return m_closed ? std::nullopt : m_state.ReleasedImage();
}

void SwapchainAccess::MarkUsed(std::uint32_t index, const std::shared_ptr<const GpuCompletion>& completion)
{
	std::lock_guard lock(m_mutex);
	if (!m_closed && completion && index < m_lastUse.size())
	{
		m_lastUse[index] = completion;
		m_changed.notify_all();
	}
}

void SwapchainAccess::MarkReleasedUsed(const std::shared_ptr<const GpuCompletion>& completion)
{
	std::lock_guard lock(m_mutex);
	if (const auto index = m_state.ReleasedImage(); !m_closed && completion && index)
	{
		m_lastUse[*index] = completion;
		m_changed.notify_all();
	}
}

void SwapchainAccess::Close()
{
	std::lock_guard lock(m_mutex);
	m_closed = true;
	m_changed.notify_all();
}

XrResult SwapchainAccess::WaitForImage(std::uint32_t index, XrDuration timeout)
{
	const auto started = std::chrono::steady_clock::now();
	while (true)
	{
		std::shared_ptr<const GpuCompletion> completion;
		{
			std::lock_guard lock(m_mutex);
			if (m_closed)
				return XR_ERROR_SESSION_LOST;
			completion = m_lastUse[index].lock();
		}
		const auto status = completion ? completion->PollCompletion() : CompletionStatus::Complete;
		std::shared_ptr<const GpuCompletion> current;
		std::unique_lock lock(m_mutex);
		if (m_closed)
			return XR_ERROR_SESSION_LOST;
		current = m_lastUse[index].lock();
		if (current != completion)
		{
			if (timeout != XR_INFINITE_DURATION &&
				std::chrono::steady_clock::now() - started >= std::chrono::nanoseconds{timeout})
				return XR_TIMEOUT_EXPIRED;
			continue;
		}
		if (status == CompletionStatus::Failed)
			return XR_ERROR_RUNTIME_FAILURE;
		if (status == CompletionStatus::Complete)
			return m_state.Wait(timeout, true);
		auto pause = std::chrono::nanoseconds{std::chrono::microseconds{100}};
		if (timeout != XR_INFINITE_DURATION)
		{
			const auto elapsed =
				std::chrono::duration_cast<std::chrono::nanoseconds>(std::chrono::steady_clock::now() - started);
			if (elapsed >= std::chrono::nanoseconds{timeout})
				return XR_TIMEOUT_EXPIRED;
			pause = std::min(pause, std::chrono::nanoseconds{timeout} - elapsed);
		}
		m_changed.wait_for(lock, pause);
	}
}

void SwapchainAccess::FinishWait()
{
	std::lock_guard lock(m_mutex);
	m_waiting = false;
	m_changed.notify_all();
}

} // namespace simxr
