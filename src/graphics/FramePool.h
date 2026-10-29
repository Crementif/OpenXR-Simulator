#pragma once

#include <cstddef>
#include <functional>
#include <memory>

namespace simxr
{
struct FramePoolState;
class FramePool;

class FrameReservation final
{
public:
	~FrameReservation();
	FrameReservation(const FrameReservation&) = delete;
	FrameReservation& operator=(const FrameReservation&) = delete;
	bool Consumed() const;

private:
	friend class FramePool;
	explicit FrameReservation(std::shared_ptr<FramePoolState> state);
	const std::shared_ptr<FramePoolState> m_state;
	bool m_acquired = false;
	bool m_consumed = false;
};

// Copies share admission and budget. Tokens keep that state alive and release
// their one slot only when their last reader disappears; no GPU work is involved.
class FramePool
{
public:
	// Reclamation may observe completed producers and release their owners, but
	// must not submit/wait for GPU work or recursively reserve from this pool.
	explicit FramePool(std::size_t capacity, std::function<void()> reclaim = {});
	explicit FramePool(std::function<std::size_t()> capacity, std::function<void()> reclaim = {});
	// Call Collect outside caller locks that native owners may reenter. A caller
	// that already collected can reserve without running that callback again.
	void Collect() const;
	std::shared_ptr<FrameReservation> Reserve(bool collect = true) const;
	bool Owns(const std::shared_ptr<FrameReservation>& reservation) const;
	bool Consume(const std::shared_ptr<FrameReservation>& reservation) const;
	void CloseAdmission() const;
	std::size_t InUse() const;
	std::size_t Capacity() const;
	// Existing reservations remain charged after shrinking.
	void SetCapacity(std::size_t capacity) const;

private:
	std::shared_ptr<FramePoolState> m_state;
};
} // namespace simxr
