#pragma once

#include "graphics/FramePool.h"
#include "presentation/FrameHistory.h"
#include "presentation/PresentationController.h"

#include <compare>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <vector>

namespace simxr
{
struct CaptureId
{
	std::uint64_t value = 0;
	auto operator<=>(const CaptureId&) const = default;
};

enum class CaptureKind
{
	ComposedEyes,
	LayerView,
	VirtualDisplay
};

struct CaptureTarget
{
	CaptureKind kind = CaptureKind::ComposedEyes;
	std::uint32_t layer = 0;
	std::uint32_t view = 0;
	auto operator<=>(const CaptureTarget&) const = default;
};

enum class SnapshotPhase
{
	Reserved,
	Issued,
	Begun,
	GpuPending,
	Ready,
	Clear,
	Empty,
	Discarded,
	Cancelled,
	Failed,
	Unavailable
};

struct SnapshotRecord
{
	CaptureId id;
	SessionGeneration generation;
	CaptureTarget target;
	std::optional<FrameKey> frame;
	SnapshotPhase phase = SnapshotPhase::Reserved;
	XrResult result = XR_SUCCESS;
	std::shared_ptr<const FrameRecord> metadata;
	std::shared_ptr<const FrameOutput> output;
	std::optional<PresentationTick> display;

  private:
	friend class SnapshotStore;
	std::shared_ptr<FrameReservation> m_retention;
};

enum class SnapshotAdmission
{
	Accepted,
	Capacity,
	Closed,
	Unsupported,
	Invalid
};

struct CaptureReservation
{
	SnapshotAdmission result;
	CaptureId id;
};

struct CaptureOutput
{
	CaptureTarget target;
	std::shared_ptr<const FrameOutput> output;
};

struct SnapshotStoreState
{
	bool running = false;
	SessionGeneration generation;
	std::size_t capacity = 0;
	std::size_t heldSlots = 0;
	std::size_t retiring = 0;
	std::vector<CaptureId> captures;
};

class SnapshotStore
{
  public:
	explicit SnapshotStore(std::size_t capacity = 32);
	~SnapshotStore();
	void Start(SessionGeneration generation);
	void Stop() noexcept;
	CaptureReservation Reserve(CaptureTarget target = {}, std::shared_ptr<FrameReservation> producer = {},
							   std::shared_ptr<FrameReservation> composedProducer = {});
	CaptureReservation Pin(std::shared_ptr<const FrameRecord> metadata,
						   std::shared_ptr<const FrameRecord> cancelled = {},
						   std::shared_ptr<const FrameOutput> output = {},
						   std::shared_ptr<FrameReservation> producer = {}, CaptureTarget target = {});
	// Bind is allocation-free. The caller supplies both immutable records prepared
	// by frame-history issue before committing scheduler admission.
	bool Bind(CaptureId id, std::shared_ptr<const FrameRecord> issued,
			  std::shared_ptr<const FrameRecord> cancelled) noexcept;
	void Update(std::shared_ptr<const FrameRecord> metadata, std::span<const CaptureOutput> outputs = {}) noexcept;
	bool PublishDisplay(CaptureId id, const PresentationTick& tick,
						std::shared_ptr<const FrameRecord> metadata = {}) noexcept;
	std::shared_ptr<FrameReservation> ProducerReservation(CaptureId id) const;
	std::shared_ptr<FrameReservation> ProducerReservation(FrameKey frame, CaptureTarget target = {}) const;
	bool SetProducer(FrameKey frame, std::shared_ptr<FrameReservation> producer, CaptureTarget target = {}) noexcept;
	void AbandonProducer(FrameKey frame, CaptureTarget target = {}) noexcept;
	std::vector<CaptureTarget> RequestedTargets(FrameKey frame) const;
	void Fail(FrameKey frame, XrResult result, std::shared_ptr<const FrameOutput> output = {}) noexcept;
	std::optional<SnapshotRecord> Read(CaptureId id) const;
	std::optional<CaptureId> Find(FrameKey frame, CaptureTarget target = {}) const;
	std::vector<CaptureId> FindAll(FrameKey frame) const;
	SnapshotStoreState Snapshot() const;
	std::size_t Capacity() const;
	bool Release(CaptureId id) noexcept;
	void RetireAll() noexcept;
	// Observes producers outside the store mutex. It submits no graphics work.
	void Poll() noexcept;

  private:
	struct Entry
	{
		SnapshotRecord record;
		std::shared_ptr<const FrameRecord> cancelled;
		std::shared_ptr<FrameReservation> producer;
		std::shared_ptr<FrameReservation> composedProducer;
		bool released = false;
		std::uint64_t revision = 0;
	};
	void CancelPendingLocked() noexcept;
	FramePool m_slots;
	mutable std::mutex m_mutex;
	bool m_running = false;
	SessionGeneration m_generation;
	std::vector<Entry> m_entries;
};
} // namespace simxr
