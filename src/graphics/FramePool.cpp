#include "graphics/FramePool.h"

#include <mutex>
#include <stdexcept>
#include <utility>

namespace simxr
{
namespace detail
{
struct FramePoolState
{
	explicit FramePoolState(std::size_t limit, std::function<void()> callback)
		: capacity(limit), reclaim(std::move(callback))
	{
	}
	std::mutex mutex;
	const std::size_t capacity;
	const std::function<void()> reclaim;
	std::size_t inUse = 0;
	bool closed = false;
};
} // namespace detail

FrameReservation::FrameReservation(std::shared_ptr<detail::FramePoolState> state) : m_state(std::move(state)) {}

FrameReservation::~FrameReservation()
{
	// shared_ptr control-block allocation may fail before Reserve accounts for
	// this object. That rollback must not relock the already-held pool mutex.
	if (!m_acquired)
		return;
	std::lock_guard lock(m_state->mutex);
	--m_state->inUse;
}

bool FrameReservation::Consumed() const
{
	std::lock_guard lock(m_state->mutex);
	return m_consumed;
}

FramePool::FramePool(std::size_t capacity, std::function<void()> reclaim)
	: m_state(std::make_shared<detail::FramePoolState>(capacity, std::move(reclaim)))
{
	if (!capacity)
		throw std::invalid_argument("Frame pool requires capacity");
}

void FramePool::Collect() const
{
	if (m_state->reclaim)
	{
		{
			std::lock_guard lock(m_state->mutex);
			if (m_state->closed)
				return;
		}
		m_state->reclaim();
	}
}

std::shared_ptr<FrameReservation> FramePool::Reserve(bool collect) const
{
	if (collect)
		Collect();
	std::lock_guard lock(m_state->mutex);
	if (m_state->closed || m_state->inUse == m_state->capacity)
		return {};
	auto reservation = std::shared_ptr<FrameReservation>(new FrameReservation(m_state));
	reservation->m_acquired = true;
	++m_state->inUse;
	return reservation;
}

bool FramePool::Owns(const std::shared_ptr<FrameReservation>& reservation) const
{
	return reservation && reservation->m_state == m_state && reservation->m_acquired;
}

bool FramePool::Consume(const std::shared_ptr<FrameReservation>& reservation) const
{
	if (!Owns(reservation))
		return false;
	std::lock_guard lock(m_state->mutex);
	if (reservation->m_consumed)
		return false;
	reservation->m_consumed = true;
	return true;
}

void FramePool::CloseAdmission() const
{
	std::lock_guard lock(m_state->mutex);
	m_state->closed = true;
}

std::size_t FramePool::InUse() const
{
	std::lock_guard lock(m_state->mutex);
	return m_state->inUse;
}

std::size_t FramePool::Capacity() const
{
	return m_state->capacity;
}
} // namespace simxr
