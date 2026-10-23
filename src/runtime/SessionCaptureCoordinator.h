#pragma once

#include "presentation/SnapshotStore.h"
#include "presentation/FrameImageHistory.h"

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
	class SequenceCapture;
	explicit SessionCaptureCoordinator(FrameHistory& history);
	~SessionCaptureCoordinator();
	XrResult Configure(FramePool&& pool, bool layerViews, CapturePromoter&& promoter, bool depthViews, bool filteredComposition);
	FrameImageHistory::Retired Start(SessionGeneration generation);
	XrResult EnableImageHistory();
	bool SupportsImageHistory() const;
	FrameImageHistory& Images() { return m_images; }
	void Stop() noexcept;
	void Retire() noexcept;
	SnapshotStore& Snapshots();
	bool Supports(CaptureTarget target = {}) const;
	// May release completed native owners. Call outside Session/caller locks.
	void Collect() const;
	void ReclaimSnapshots(std::size_t producers, std::size_t slots);
	CaptureBoundaryResult ReserveBundle(std::span<const CaptureTarget> targets);
	SequenceCapture PrepareSequenceCapture(const std::optional<SequenceCaptureIntent>& request);
	std::pair<XrResult, std::shared_ptr<FrameReservation>> PrepareProducer(FrameKey frame, CaptureTarget target);
	SnapshotAdmission StartImageRecording(CaptureTarget target, std::uint32_t capacity, std::uint64_t frames = 0,
		std::span<CaptureId> captures = {});
	void StopImageRecording() noexcept;
	ImageRecordingSnapshot ImageRecording() const;
	CaptureId RefillImageRecording(CaptureId released);
	void BindIssued(std::shared_ptr<const FrameRecord> issued, std::shared_ptr<const FrameRecord> cancelled);
	void ObserveFrame(FrameKey frame);
	std::size_t CancelQueued() noexcept;
	Submission PrepareSubmission(std::shared_ptr<const FrameOutput> output, std::span<const CaptureOutput> captures);
	Boundary PrepareBoundary();

  private:
	bool SupportsLocked(CaptureTarget target) const;
	CaptureReservation ReserveLocked(CaptureTarget target);
	void StopImageRecordingLocked() noexcept;
	bool HasRequest(FrameKey frame, CaptureTarget target) const;
	std::size_t CancelQueuedLocked() noexcept;
	SnapshotAdmission Promote(const std::shared_ptr<const FrameOutput>& output,
							  std::shared_ptr<FrameReservation> producer);
	FrameHistory& m_history;
	mutable std::mutex m_mutex;
	std::optional<FramePool> m_pool;
	bool m_layerViews = false;
	bool m_depthViews = false;
	bool m_filteredComposition = false;
	bool m_imageHistory = false;
	FrameImageHistory m_images;
	CapturePromoter m_promoter;
	bool m_running = false;
	SessionGeneration m_generation;
	std::vector<std::vector<CaptureId>> m_queued;
	ImageRecordingSnapshot m_imageRecording;
	std::vector<CaptureId> m_recordingQueued;
	// Destroy the store before configuration/mutex so reentrant last readers can
	// still inspect this coordinator while SnapshotStore detaches its entries.
	SnapshotStore m_store;
};

// Reserves before history publication. Failed issue releases the unused producer;
// Bind runs last in the issue callback, after every throwing admission step.
class SessionCaptureCoordinator::SequenceCapture
{
  public:
	SequenceCapture(SessionCaptureCoordinator& owner, const std::optional<SequenceCaptureIntent>& request);
	~SequenceCapture();
	SequenceCapture(const SequenceCapture&) = delete;
	SequenceCapture& operator=(const SequenceCapture&) = delete;
	const std::optional<SequenceCaptureRecord>& Record() const noexcept;
	void Bind(std::shared_ptr<const FrameRecord> issued, std::shared_ptr<const FrameRecord> cancelled);

  private:
	SessionCaptureCoordinator& m_owner;
	std::optional<SequenceCaptureRecord> m_record;
	bool m_committed = false;
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
	FrameImageHistory::Retired m_retiredImage;
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
	FrameImageHistory::Retired m_retiredImages;
	std::vector<PresentationSubmission> m_retained;
	std::vector<Source> m_sources;
	std::vector<CaptureId> m_created;
	std::vector<CaptureId> m_existing;
	CaptureBoundaryResult m_result;
	bool m_committed = false;
};
} // namespace simxr
