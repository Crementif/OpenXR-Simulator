#pragma once

#include "presentation/SnapshotStore.h"

#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <utility>
#include <vector>

namespace simxr
{
struct FramePipelineState;

// Invoked under the caller's Session lock: CPU-only, no reentry/Collect/waits
// and no output/native-owner destruction. Budget reclassification may persist
// after a failed boundary; its actual image owners continue to exist.
using CapturePromoter = std::function<SnapshotAdmission(const std::shared_ptr<const FrameOutput>&, const FramePool&,
														std::shared_ptr<FrameReservation>)>;

struct CaptureBoundaryResult
{
	SnapshotAdmission result = SnapshotAdmission::Closed;
	std::vector<CaptureId> captures;
};

// Owns capture admission and retention. History outlives this owner. No method
// takes a Session lock; the Session serializes lifecycle and frame acceptance.
// The internal lock is never held while collecting, looking up history or
// destroying the last native output reader.
class SessionCaptureCoordinator
{
  public:
	class Submission;
	class Boundary;
	explicit SessionCaptureCoordinator(FrameHistory& history);
	~SessionCaptureCoordinator();
	XrResult Configure(FramePool&& pool, bool layerViews, CapturePromoter&& promoter);
	void Start(SessionGeneration generation);
	void Stop() noexcept;
	void Retire() noexcept;
	SnapshotStore& Snapshots();
	bool Supports(CaptureTarget target = {}) const;
	// May release completed native owners. Call outside Session/caller locks.
	void Collect() const;
	CaptureReservation Reserve(CaptureTarget target);
	std::pair<XrResult, std::shared_ptr<FrameReservation>> PrepareProducer(FrameKey frame, CaptureTarget target);
	void BindIssued(std::shared_ptr<const FrameRecord> issued, std::shared_ptr<const FrameRecord> cancelled);
	void ObserveFrame(FrameKey frame);
	std::size_t CancelQueued() noexcept;
	Submission PrepareSubmission(std::shared_ptr<const FrameOutput> output, std::span<const CaptureOutput> captures);
	Boundary PrepareBoundary();

  private:
	bool SupportsLocked(CaptureTarget target) const;
	bool HasRequest(FrameKey frame, CaptureTarget target) const;
	std::size_t CancelQueuedLocked() noexcept;
	SnapshotAdmission Promote(const std::shared_ptr<const FrameOutput>& output,
							  std::shared_ptr<FrameReservation> producer);
	FrameHistory& m_history;
	mutable std::mutex m_mutex;
	std::optional<FramePool> m_pool;
	bool m_layerViews = false;
	CapturePromoter m_promoter;
	bool m_running = false;
	SessionGeneration m_generation;
	std::vector<CaptureId> m_queued;
	// Destroy the store before configuration/mutex so reentrant last readers can
	// still inspect this coordinator while SnapshotStore detaches its entries.
	SnapshotStore m_store;
};

// Construct before taking the Session lock. The retained outputs and rollback
// token readers then outlive every protected frame-acceptance scope.
class SessionCaptureCoordinator::Submission
{
  public:
	Submission(SessionCaptureCoordinator& owner, std::shared_ptr<const FrameOutput> output,
			   std::span<const CaptureOutput> captures);
	~Submission();
	Submission(const Submission&) = delete;
	Submission& operator=(const Submission&) = delete;
	XrResult Validation() const;
	XrResult Prepare(FrameKey frame);
	XrResult Promote();
	bool Requested() const;
	void Commit(std::shared_ptr<const FrameRecord> metadata, bool includeOutputs) noexcept;

  private:
	SessionCaptureCoordinator& m_owner;
	std::shared_ptr<const FrameOutput> m_output;
	std::vector<CaptureOutput> m_outputs;
	std::vector<CaptureTarget> m_requested;
	std::vector<std::pair<CaptureTarget, std::shared_ptr<FrameReservation>>> m_promoted;
	FrameKey m_frame;
	XrResult m_validation = XR_SUCCESS;
	bool m_committed = false;
};

// Retains all selected native readers until after the caller's lock is gone.
// Failed pinning rolls back only records created by this boundary.
class SessionCaptureCoordinator::Boundary
{
  public:
	explicit Boundary(SessionCaptureCoordinator& owner);
	~Boundary();
	Boundary(const Boundary&) = delete;
	Boundary& operator=(const Boundary&) = delete;
	CaptureBoundaryResult Capture(SessionGeneration generation, const FramePipelineState& pipeline,
								  std::vector<PresentationSubmission> retained, std::optional<FrameKey> after);

  private:
	struct Source
	{
		std::shared_ptr<const FrameRecord> metadata;
		std::shared_ptr<const FrameRecord> cancelled;
		std::shared_ptr<const FrameOutput> output;
	};
	void Rollback() noexcept;
	SessionCaptureCoordinator& m_owner;
	std::vector<PresentationSubmission> m_retained;
	std::vector<Source> m_sources;
	std::vector<CaptureId> m_created;
	std::vector<CaptureId> m_existing;
	CaptureBoundaryResult m_result;
	bool m_committed = false;
};
} // namespace simxr
