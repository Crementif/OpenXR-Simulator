#include "graphics/SwapchainWaits.h"
#include "Pch.h"

namespace simxr
{
SwapchainWaits::Registration::Registration(SwapchainWaits& owner) : m_owner(&owner) {}
SwapchainWaits::Registration::Registration(Registration&& other) noexcept
	: m_owner(std::exchange(other.m_owner, nullptr))
{
}
SwapchainWaits::Registration::~Registration()
{
	if (m_owner)
		m_owner->Leave();
}

std::optional<SwapchainWaits::Registration> SwapchainWaits::Enter()
{
	std::lock_guard lock(m_mutex);
	if (m_closed)
		return {};
	++m_readers;
	return Registration{*this};
}

void SwapchainWaits::Close()
{
	std::lock_guard lock(m_mutex);
	m_closed = true;
	m_changed.notify_all();
}

bool SwapchainWaits::Drain(std::chrono::nanoseconds timeout)
{
	std::unique_lock lock(m_mutex);
	return m_changed.wait_for(lock, timeout,
							  [this]
							  {
								  return m_readers == 0;
							  });
}

void SwapchainWaits::Leave()
{
	std::lock_guard lock(m_mutex);
	--m_readers;
	m_changed.notify_all();
}
} // namespace simxr
