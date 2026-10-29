#pragma once

#include "graphics/FramePool.h"
#include "presentation/CaptureTypes.h"
#include "presentation/FrameHistory.h"
#include "presentation/PresentationController.h"

#include <array>
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
enum class CaptureKind
{
	ComposedEyes,
	LayerView,
	VirtualDisplay,
	LayerDepth,
	LayerDepthLinear,
	ProjectionLayers,
	QuadLayers
};

struct CaptureTarget
{
	CaptureKind kind = CaptureKind::ComposedEyes;
	std::uint32_t layer = 0;
	std::uint32_t view = 0;

	bool IsDepth() const
	{
		return kind == CaptureKind::LayerDepth || kind == CaptureKind::LayerDepthLinear;
	}

	bool IsLayerImage() const
	{
		return kind == CaptureKind::LayerView || IsDepth();
	}

	bool IsFilteredComposition() const
	{
		return kind == CaptureKind::ProjectionLayers || kind == CaptureKind::QuadLayers;
	}

	bool IsAdditionalImage() const
	{
		return IsLayerImage() || IsFilteredComposition();
	}

	auto operator<=>(const CaptureTarget&) const = default;
};

enum class ImageRecordingPhase
{
	Idle,
	Recording,
	Stopped,
	Overrun
};

struct ImageRecordingSnapshot
{
	ImageRecordingPhase phase = ImageRecordingPhase::Idle;
	CaptureTarget target;
	std::uint32_t capacity = 0;
	std::uint64_t captured = 0;
	std::uint64_t requested = 0;
	std::optional<FrameKey> missed;
	std::vector<CaptureId> captures;
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
	bool pinned = false;
	std::shared_ptr<const FrameRecord> metadata;
	std::shared_ptr<const FrameOutput> output;
	std::optional<PresentationTick> display;

private:
	friend class SnapshotStore;
	std::shared_ptr<FrameReservation> m_retention;
};

struct CaptureOutput
{
	CaptureTarget target;
	std::shared_ptr<const FrameOutput> output;
	std::optional<std::array<XrRect2Di, 2>> quadBounds;
};

struct SnapshotStoreState
{
	bool running = false;
	SessionGeneration generation;
	std::size_t capacity = 0;
	std::size_t heldSlots = 0;
	std::size_t retiring = 0;
	std::size_t pinned = 0;
	std::vector<CaptureId> captures;
};

class SnapshotStore
{
public:
	// History outlives the store and retains no image or reservation readers.
	explicit SnapshotStore(std::size_t capacity = 0, FrameHistory* history = nullptr);
	~SnapshotStore();
	void Start(SessionGeneration generation);
	void Stop() noexcept;
	CaptureReservation Reserve(CaptureTarget target = {}, std::shared_ptr<FrameReservation> producer = {},
							   std::shared_ptr<FrameReservation> composedProducer = {});
	CaptureReservation Pin(const std::shared_ptr<const FrameRecord>& metadata,
						   std::shared_ptr<const FrameRecord> cancelled = {},
						   std::shared_ptr<const FrameOutput> output = {},
						   std::shared_ptr<FrameReservation> producer = {}, CaptureTarget target = {});
	// Bind is allocation-free. The caller supplies both immutable records prepared
	// by frame-history issue before committing scheduler admission.
	bool Bind(CaptureId id, std::shared_ptr<const FrameRecord> issued,
			  std::shared_ptr<const FrameRecord> cancelled) noexcept;
	void Update(const std::shared_ptr<const FrameRecord>& metadata,
				std::span<const CaptureOutput> outputs = {}) noexcept;
	bool PublishDisplay(CaptureId id, const PresentationTick& tick,
						std::shared_ptr<const FrameRecord> metadata = {}) noexcept;
	std::shared_ptr<FrameReservation> ProducerReservation(CaptureId id) const;
	std::shared_ptr<FrameReservation> ProducerReservation(FrameKey frame, CaptureTarget target = {}) const;
	bool SetProducer(FrameKey frame, const std::shared_ptr<FrameReservation>& producer,
					 CaptureTarget target = {}) noexcept;
	void AbandonProducer(FrameKey frame, CaptureTarget target = {}) noexcept;
	std::vector<CaptureTarget> RequestedTargets(FrameKey frame) const;
	void Fail(FrameKey frame, XrResult result, const std::shared_ptr<const FrameOutput>& output = {}) noexcept;
	std::optional<SnapshotRecord> Read(CaptureId id) const;
	std::optional<CaptureId> Find(FrameKey frame, CaptureTarget target = {}) const;
	std::vector<CaptureId> FindAll(FrameKey frame) const;
	SnapshotStoreState Snapshot() const;
	std::size_t Capacity() const;
	bool Release(CaptureId id) noexcept;
	bool SetPinned(CaptureId id, bool pinned) noexcept;
	// Call outside caller locks: eviction releases native owners through normal retirement.
	bool EvictOldest() noexcept;
	void SetCapacity(std::size_t capacity);
	void RetireAll() noexcept;
	// Marks only sequence-owned tickets; Poll releases native readers outside caller locks.
	void RetireSequenceCaptures(std::optional<CommandSource> owner = {}) noexcept;
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
		bool terminalTraced = false;
		std::uint64_t revision = 0;
	};

	std::optional<Entry> ReleaseLocked(std::vector<Entry>::iterator entry) noexcept;
	void CancelPendingLocked() noexcept;
	void TraceTerminal(Entry& entry) noexcept;
	FrameHistory* const m_history;
	FramePool m_slots;
	mutable std::mutex m_mutex;
	bool m_running = false;
	SessionGeneration m_generation;
	std::vector<Entry> m_entries;
};
} // namespace simxr
