#include "graphics/FramePool.h"
#include "Pch.h"

namespace simxr
{
struct FramePoolState
{
	explicit FramePoolState(std::size_t limit, std::function<void()> callback)
		: capacity(limit), reclaim(std::move(callback))
	{}

	std::mutex mutex;
	std::size_t capacity;
	std::function<std::size_t()> capacityProvider;
	const std::function<void()> reclaim;
	std::size_t inUse = 0;
	bool closed = false;
};

FrameReservation::FrameReservation(std::shared_ptr<FramePoolState> state) : m_state(std::move(state)) {}

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
	: m_state(std::make_shared<FramePoolState>(capacity, std::move(reclaim)))
{
	if (!capacity)
		throw std::invalid_argument("Frame pool requires capacity");
}

FramePool::FramePool(std::function<std::size_t()> capacity, std::function<void()> reclaim)
	: FramePool(1, std::move(reclaim))
{
	if (!capacity)
		throw std::invalid_argument("Frame pool requires a capacity provider");
	m_state->capacityProvider = std::move(capacity);
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
	if (m_state->closed ||
		m_state->inUse >= (m_state->capacityProvider ? m_state->capacityProvider() : m_state->capacity))
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
	std::lock_guard lock(m_state->mutex);
	return m_state->capacityProvider ? m_state->capacityProvider() : m_state->capacity;
}

void FramePool::SetCapacity(std::size_t capacity) const
{
	if (!capacity)
		throw std::invalid_argument("Frame pool requires capacity");
	std::lock_guard lock(m_state->mutex);
	m_state->capacityProvider = {};
	m_state->capacity = capacity;
}
} // namespace simxr
