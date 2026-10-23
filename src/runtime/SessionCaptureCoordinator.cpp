#include "runtime/SessionCaptureCoordinator.h"
#include "presentation/FrameScheduler.h"

#include <algorithm>
#include <stdexcept>
#include <utility>

namespace simxr
{
SessionCaptureCoordinator::SessionCaptureCoordinator(FrameHistory& history) : m_history(history), m_store(0, &history)
{
	m_queued.reserve(m_store.Capacity());
	m_imageRecording.captures.reserve(4);
	m_recordingQueued.reserve(4);
}

SessionCaptureCoordinator::~SessionCaptureCoordinator()
{
	CapturePromoter promoter;
	std::optional<FramePool> pool;
	{
		std::lock_guard lock(m_mutex);
		m_running = false;
		promoter = std::move(m_promoter);
		pool = std::move(m_pool);
		m_pool.reset();
	}
	m_store.Stop();
	// Release callback captures outside the lock while the store and mutex live.
}

XrResult SessionCaptureCoordinator::Configure(FramePool&& pool, bool layerViews, CapturePromoter&& promoter, bool depthViews,
	bool filteredComposition)
{
	std::lock_guard lock(m_mutex);
	if (m_running || m_pool)
		return XR_ERROR_CALL_ORDER_INVALID;
	m_pool = std::move(pool);
	m_layerViews = layerViews;
	m_depthViews = layerViews && depthViews;
	m_filteredComposition = filteredComposition;
	m_promoter = std::move(promoter);
	return XR_SUCCESS;
}

XrResult SessionCaptureCoordinator::EnableImageHistory()
{
	std::lock_guard lock(m_mutex);
	if (m_running)
		return XR_ERROR_CALL_ORDER_INVALID;
	if (!m_pool || !m_promoter)
		return XR_ERROR_FEATURE_UNSUPPORTED;
	m_imageHistory = true;
	return XR_SUCCESS;
}
bool SessionCaptureCoordinator::SupportsImageHistory() const
{
	std::lock_guard lock(m_mutex);
	return m_imageHistory;
}
FrameImageHistory::Retired SessionCaptureCoordinator::Start(SessionGeneration generation)
{
	std::lock_guard lock(m_mutex);
	m_store.Start(generation);
	m_generation = generation;
	m_imageRecording.phase = ImageRecordingPhase::Idle;
	m_imageRecording.target = {};
	m_imageRecording.capacity = 0;
	m_imageRecording.captured = 0;
	m_imageRecording.requested = 0;
	m_imageRecording.missed.reset();
	m_imageRecording.captures.clear();
	m_recordingQueued.clear();
	m_running = true;
	return m_images.Start(generation);
}

void SessionCaptureCoordinator::Stop() noexcept
{
	std::lock_guard lock(m_mutex);
	m_running = false;
	StopImageRecordingLocked();
	CancelQueuedLocked();
	m_store.Stop();
}

void SessionCaptureCoordinator::Retire() noexcept
{
	m_store.RetireAll();
}

SnapshotStore& SessionCaptureCoordinator::Snapshots()
{
	return m_store;
}

bool SessionCaptureCoordinator::SupportsLocked(CaptureTarget target) const
{
	return m_pool && (target == CaptureTarget{} || (m_layerViews && target.kind == CaptureKind::LayerView) ||
		(m_depthViews && target.IsDepth()) ||
		(m_filteredComposition && target.IsFilteredComposition() && !target.layer && !target.view));
}

bool SessionCaptureCoordinator::Supports(CaptureTarget target) const
{
	std::lock_guard lock(m_mutex);
	return SupportsLocked(target);
}

void SessionCaptureCoordinator::Collect() const
{
	std::optional<FramePool> pool;
	{
		std::lock_guard lock(m_mutex);
		pool = m_pool;
	}
	if (pool)
		pool->Collect();
}

void SessionCaptureCoordinator::ReclaimSnapshots(std::size_t producers, std::size_t slots)
{
	Collect();
	m_store.Poll();
	std::optional<FramePool> pool;
	{
		std::lock_guard lock(m_mutex);
		pool = m_pool;
	}
	if (slots > m_store.Capacity() || (pool && producers > pool->Capacity()))
		return;
	while (m_store.Snapshot().heldSlots + slots > m_store.Capacity() ||
		(pool && pool->InUse() + producers > pool->Capacity()))
	{
		if (!m_store.EvictOldest())
			break;
		if (pool)
			pool->Collect();
	}
}

CaptureBoundaryResult SessionCaptureCoordinator::ReserveBundle(std::span<const CaptureTarget> targets)
{
	if (targets.empty() || targets.size() > 8)
		return {SnapshotAdmission::Invalid, {}};
	std::lock_guard lock(m_mutex);
	for (std::size_t index = 0; index < targets.size(); ++index)
	{
		if (!SupportsLocked(targets[index]))
			return {SnapshotAdmission::Unsupported, {}};
		if (std::find(targets.begin(), targets.begin() + index, targets[index]) != targets.begin() + index)
			return {SnapshotAdmission::Invalid, {}};
	}
	CaptureBoundaryResult result{SnapshotAdmission::Accepted, {}};
	result.captures.reserve(targets.size());
	m_queued.reserve(m_queued.size() + 1);
	try
	{
		for (const auto target : targets)
		{
			const auto reserved = ReserveLocked(target);
			if (reserved.result != SnapshotAdmission::Accepted)
			{
				for (const auto id : result.captures)
					m_store.Release(id);
				return {reserved.result, {}};
			}
			result.captures.push_back(reserved.id);
		}
		m_queued.push_back(result.captures);
	}
	catch (...)
	{
		for (const auto id : result.captures)
			m_store.Release(id);
		throw;
	}
	return result;
}

SessionCaptureCoordinator::SequenceCapture SessionCaptureCoordinator::PrepareSequenceCapture(
	const std::optional<SequenceCaptureIntent>& request)
{
	return SequenceCapture(*this, request);
}
SessionCaptureCoordinator::SequenceCapture::SequenceCapture(SessionCaptureCoordinator& owner,
	const std::optional<SequenceCaptureIntent>& request) : m_owner(owner)
{
	if (!request)
		return;
	std::lock_guard lock(owner.m_mutex);
	m_record = SequenceCaptureRecord{*request, owner.ReserveLocked({})};
}
SessionCaptureCoordinator::SequenceCapture::~SequenceCapture()
{
	if (!m_committed && m_record && m_record->reservation.result == SnapshotAdmission::Accepted)
		m_owner.m_store.Release(m_record->reservation.id);
}
const std::optional<SequenceCaptureRecord>& SessionCaptureCoordinator::SequenceCapture::Record() const noexcept
{
	return m_record;
}
void SessionCaptureCoordinator::SequenceCapture::Bind(std::shared_ptr<const FrameRecord> issued,
	std::shared_ptr<const FrameRecord> cancelled)
{
	if (!m_record)
		return;
	if (m_record->reservation.result == SnapshotAdmission::Accepted &&
		!m_owner.m_store.Bind(m_record->reservation.id, issued, std::move(cancelled)))
		throw std::logic_error("Sequence capture reservation was cancelled before admission");
	m_committed = true;
	XrResult result = XR_SUCCESS;
	switch (m_record->reservation.result)
	{
	case SnapshotAdmission::Accepted:
		return;
	case SnapshotAdmission::Capacity:
		result = XR_ERROR_LIMIT_REACHED;
		break;
	case SnapshotAdmission::Closed:
		result = XR_ERROR_SESSION_NOT_RUNNING;
		break;
	case SnapshotAdmission::Unsupported:
		result = XR_ERROR_FEATURE_UNSUPPORTED;
		break;
	case SnapshotAdmission::Invalid:
		result = XR_ERROR_VALIDATION_FAILURE;
		break;
	}
	m_owner.m_history.Trace(FrameTraceKind::CaptureFailed, issued->key, issued->prediction.displayTime, result,
		result == XR_ERROR_LIMIT_REACHED ? FrameDiagnosticCategory::Performance : FrameDiagnosticCategory::ApiError,
		0, "sequence_capture_admission");
}

CaptureReservation SessionCaptureCoordinator::ReserveLocked(CaptureTarget target)
{
	if (!m_running)
		return {SnapshotAdmission::Closed, {}};
	if (!SupportsLocked(target))
		return {SnapshotAdmission::Unsupported, {}};
	if (m_queued.size() == m_store.Capacity())
		return {SnapshotAdmission::Capacity, {}};
	auto producer = m_pool->Reserve(false);
	if (!producer)
		return {SnapshotAdmission::Capacity, {}};
	std::shared_ptr<FrameReservation> composedProducer;
	if (target.IsAdditionalImage())
	{
		composedProducer = m_pool->Reserve(false);
		if (!composedProducer)
			return {SnapshotAdmission::Capacity, {}};
	}
	return m_store.Reserve(target, std::move(producer), std::move(composedProducer));
}

bool SessionCaptureCoordinator::HasRequest(FrameKey frame, CaptureTarget target) const
{
	for (const auto& requested : m_store.RequestedTargets(frame))
	{
		if (requested == target)
			return true;
		if (target == CaptureTarget{} && requested.IsAdditionalImage())
			return true;
	}
	return false;
}

std::pair<XrResult, std::shared_ptr<FrameReservation>> SessionCaptureCoordinator::PrepareProducer(FrameKey frame,
																								  CaptureTarget target)
{
	std::lock_guard lock(m_mutex);
	if (!m_running || frame.generation != m_generation)
		return {XR_ERROR_SESSION_NOT_RUNNING, {}};
	if (target != CaptureTarget{} && !SupportsLocked(target))
		return {XR_ERROR_FEATURE_UNSUPPORTED, {}};
	auto producer = m_store.ProducerReservation(frame, target);
	if (producer || !HasRequest(frame, target))
		return {XR_SUCCESS, std::move(producer)};
	if (!m_pool)
		return {XR_ERROR_FEATURE_UNSUPPORTED, {}};
	auto replacement = m_pool->Reserve(false);
	if (!replacement)
		return {XR_ERROR_LIMIT_REACHED, {}};
	if (!m_store.SetProducer(frame, replacement, target))
	{
		// A direct store release may abandon a request during native preparation.
		if (!HasRequest(frame, target))
			return {XR_SUCCESS, {}};
		return {XR_ERROR_RUNTIME_FAILURE, {}};
	}
	return {XR_SUCCESS, std::move(replacement)};
}

SnapshotAdmission SessionCaptureCoordinator::StartImageRecording(CaptureTarget target, std::uint32_t capacity, std::uint64_t frames,
	std::span<CaptureId> captures)
{
	std::lock_guard lock(m_mutex);
	if (!m_running)
		return SnapshotAdmission::Closed;
	if (!SupportsLocked(target))
		return SnapshotAdmission::Unsupported;
	if (!capacity || capacity > 4 || (!captures.empty() && captures.size() != capacity) || !m_queued.empty() || !m_imageRecording.captures.empty())
		return SnapshotAdmission::Invalid;
	m_imageRecording.target = target;
	m_imageRecording.capacity = capacity;
	m_imageRecording.captured = 0;
	m_imageRecording.requested = frames;
	m_imageRecording.missed.reset();
	try
	{
		for (std::uint32_t index = 0; index < capacity; ++index)
		{
			const auto reserved = ReserveLocked(target);
			if (reserved.result != SnapshotAdmission::Accepted)
			{
				StopImageRecordingLocked();
				return reserved.result;
			}
			m_recordingQueued.push_back(reserved.id);
			m_imageRecording.captures.push_back(reserved.id);
		}
	}
	catch (...)
	{
		StopImageRecordingLocked();
		throw;
	}
	if (!captures.empty())
		std::copy(m_imageRecording.captures.begin(), m_imageRecording.captures.end(), captures.begin());
	m_imageRecording.phase = ImageRecordingPhase::Recording;
	return SnapshotAdmission::Accepted;
}

void SessionCaptureCoordinator::StopImageRecordingLocked() noexcept
{
	if (m_imageRecording.phase != ImageRecordingPhase::Overrun)
		m_imageRecording.phase = ImageRecordingPhase::Stopped;
	for (const auto capture : m_recordingQueued)
	{
		m_store.Release(capture); // Unissued reservations have no native image reader.
		std::erase(m_imageRecording.captures, capture);
	}
	m_recordingQueued.clear();
}

void SessionCaptureCoordinator::StopImageRecording() noexcept
{
	std::lock_guard lock(m_mutex);
	StopImageRecordingLocked();
}

ImageRecordingSnapshot SessionCaptureCoordinator::ImageRecording() const
{
	std::lock_guard lock(m_mutex);
	return m_imageRecording;
}

CaptureId SessionCaptureCoordinator::RefillImageRecording(CaptureId released)
{
	std::lock_guard lock(m_mutex);
	const auto found = std::find(m_imageRecording.captures.begin(), m_imageRecording.captures.end(), released);
	if (found == m_imageRecording.captures.end())
		return {};
	m_imageRecording.captures.erase(found);
	if (m_imageRecording.phase != ImageRecordingPhase::Recording)
		return {};
	CaptureReservation reserved;
	try
	{
		reserved = ReserveLocked(m_imageRecording.target);
	}
	catch (...)
	{
		m_imageRecording.phase = ImageRecordingPhase::Overrun;
		StopImageRecordingLocked();
		throw;
	}
	if (reserved.result != SnapshotAdmission::Accepted)
	{
		m_imageRecording.phase = ImageRecordingPhase::Overrun;
		StopImageRecordingLocked();
		return {};
	}
	m_recordingQueued.push_back(reserved.id);
	m_imageRecording.captures.push_back(reserved.id);
	return reserved.id;
}

void SessionCaptureCoordinator::BindIssued(std::shared_ptr<const FrameRecord> issued,
										   std::shared_ptr<const FrameRecord> cancelled)
{
	std::lock_guard lock(m_mutex);
	if (!m_queued.empty())
	{
		for (const auto id : m_queued.front())
			if (!m_store.Bind(id, issued, cancelled))
				throw std::logic_error("Capture reservation was cancelled before admission");
		m_queued.erase(m_queued.begin());
	}
	if (m_imageRecording.phase != ImageRecordingPhase::Recording)
		return;
	if (m_recordingQueued.empty())
	{
		m_imageRecording.phase = ImageRecordingPhase::Overrun;
		m_imageRecording.missed = issued->key;
		m_history.Trace(FrameTraceKind::CaptureFailed, issued->key, issued->prediction.displayTime,
			XR_ERROR_LIMIT_REACHED, FrameDiagnosticCategory::Performance, 0, "image_recording_overrun");
		return;
	}
	if (!m_store.Bind(m_recordingQueued.front(), issued, cancelled))
		throw std::logic_error("Recording reservation was cancelled before admission");
	m_recordingQueued.erase(m_recordingQueued.begin());
	++m_imageRecording.captured;
	if (m_imageRecording.requested && m_imageRecording.captured == m_imageRecording.requested)
		StopImageRecordingLocked();
}

void SessionCaptureCoordinator::ObserveFrame(FrameKey frame)
{
	// BindIssued is called with history locked; never reverse that lock order.
	m_store.Update(m_history.Find(frame));
}

std::size_t SessionCaptureCoordinator::CancelQueuedLocked() noexcept
{
	const auto count = m_queued.size();
	for (const auto& bundle : m_queued)
		for (const auto id : bundle)
			m_store.Release(id); // Queued records have no output/native reader.
	m_queued.clear();
	return count;
}

std::size_t SessionCaptureCoordinator::CancelQueued() noexcept
{
	std::lock_guard lock(m_mutex);
	return CancelQueuedLocked();
}

SnapshotAdmission SessionCaptureCoordinator::Promote(const std::shared_ptr<const FrameOutput>& output,
													 std::shared_ptr<FrameReservation> producer)
{
	std::lock_guard lock(m_mutex);
	if (!m_pool)
		return SnapshotAdmission::Unsupported;
	return m_promoter ? m_promoter(output, *m_pool, std::move(producer)) : SnapshotAdmission::Accepted;
}

SessionCaptureCoordinator::Submission
SessionCaptureCoordinator::PrepareSubmission(std::shared_ptr<const FrameOutput> output,
											 std::span<const CaptureOutput> captures)
{
	return Submission(*this, std::move(output), captures);
}

SessionCaptureCoordinator::Boundary SessionCaptureCoordinator::PrepareBoundary()
{
	return Boundary(*this);
}

SessionCaptureCoordinator::Submission::Submission(SessionCaptureCoordinator& owner,
												  std::shared_ptr<const FrameOutput> output,
												  std::span<const CaptureOutput> captures)
	: m_owner(owner), m_output(std::move(output))
{
	if (captures.size() > m_owner.m_store.Capacity())
	{
		m_validation = XR_ERROR_VALIDATION_FAILURE;
		return;
	}
	m_outputs.assign(captures.begin(), captures.end());
	for (std::size_t index = 0; index < m_outputs.size(); ++index)
	{
		const auto& capture = m_outputs[index];
		if (!capture.target.IsAdditionalImage() || std::any_of(m_outputs.begin(), m_outputs.begin() + index,
																		 [&](const auto& prior)
																		 {
																			 return prior.target == capture.target;
																		 }))
		{
			m_validation = XR_ERROR_VALIDATION_FAILURE;
			return;
		}
	}
	m_outputs.push_back({{}, m_output});
	m_promoted.reserve(m_owner.m_store.Capacity() + 1);
}

SessionCaptureCoordinator::Submission::~Submission()
{
	if (m_committed)
		return;
	for (const auto& [target, producer] : m_promoted)
	{
		if (producer->Consumed() && m_owner.m_store.ProducerReservation(m_frame, target) == producer)
			m_owner.m_store.AbandonProducer(m_frame, target);
	}
}

XrResult SessionCaptureCoordinator::Submission::Validation() const
{
	return m_validation;
}

XrResult SessionCaptureCoordinator::Submission::Prepare(FrameKey frame)
{
	if (XR_FAILED(m_validation))
		return m_validation;
	for (const auto& capture : m_outputs)
		if (capture.target.IsAdditionalImage() && !m_owner.Supports(capture.target))
			return XR_ERROR_FEATURE_UNSUPPORTED;
	m_frame = frame;
	m_requested = m_owner.m_store.RequestedTargets(frame);
	return XR_SUCCESS;
}

XrResult SessionCaptureCoordinator::Submission::Promote()
{
	if (m_requested.empty())
		return XR_SUCCESS;
	for (const auto& captured : m_outputs)
	{
		if (!captured.output)
			continue;
		const auto target = captured.target;
		auto producer = m_owner.m_store.ProducerReservation(m_frame, target);
		if (std::find(m_requested.begin(), m_requested.end(), target) == m_requested.end() &&
			!(target == CaptureTarget{} && producer))
			continue;
		if (producer && !producer->Consumed())
			m_promoted.push_back({target, producer});
		const auto promoted = m_owner.Promote(captured.output, std::move(producer));
		if (promoted != SnapshotAdmission::Accepted)
		{
			return promoted == SnapshotAdmission::Capacity		? XR_ERROR_LIMIT_REACHED
				   : promoted == SnapshotAdmission::Unsupported ? XR_ERROR_FEATURE_UNSUPPORTED
																: XR_ERROR_RUNTIME_FAILURE;
		}
	}
	return XR_SUCCESS;
}

bool SessionCaptureCoordinator::Submission::Requested() const
{
	return !m_requested.empty();
}

void SessionCaptureCoordinator::Submission::Commit(std::shared_ptr<const FrameRecord> metadata,
												   bool includeOutputs) noexcept
{
	m_committed = true;
	if (m_owner.m_imageHistory)
	{
		HistoricalImage image{metadata, {}};
		if (includeOutputs && m_output && m_requested.empty() && metadata && metadata->submission)
		{
			const auto& views = metadata->submission->viewSettings;
			const auto bytes = static_cast<std::uint64_t>(views.outerWidth) * views.outerHeight * 8;
			try
			{
				if (bytes && bytes <= FrameImageHistory::MaxImageBytes &&
					m_owner.m_promoter(m_output, m_owner.m_images.Budget(), {}) == SnapshotAdmission::Accepted)
					image.output = m_output;
			}
			catch (...)
			{
				// Optional history cannot fail accepted application work; the missing output is recorded below.
			}
		}
		if (metadata && metadata->submission && metadata->submission->layers)
			image.quadVisible = std::any_of(metadata->submission->layers->layers.begin(),
				metadata->submission->layers->layers.end(), [](const auto& layer)
				{ return std::holds_alternative<QuadLayerDescription>(layer); });
		for (const auto& captured : m_outputs)
		{
			if (captured.target.kind != CaptureKind::QuadLayers)
				continue;
			if (captured.quadBounds)
			{
				image.quadBounds = *captured.quadBounds;
				image.quadVisible = std::any_of(image.quadBounds.begin(), image.quadBounds.end(),
					[](const auto& rectangle) { return rectangle.extent.width > 0 && rectangle.extent.height > 0; });
			}
			if (includeOutputs && std::find(m_requested.begin(), m_requested.end(), captured.target) == m_requested.end())
				image.quadOutput = captured.output;
		}
		m_retiredImage = m_owner.m_images.Append(std::move(image));
	}
	m_owner.m_store.Update(std::move(metadata), includeOutputs ? std::span<const CaptureOutput>{m_outputs}
															   : std::span<const CaptureOutput>{});
}

SessionCaptureCoordinator::Boundary::Boundary(SessionCaptureCoordinator& owner)
	: m_owner(owner), m_retiredImages(owner.m_images.Clear())
{
	m_existing.reserve(m_owner.m_store.Capacity());
	m_result.captures.reserve(m_owner.m_store.Capacity());
}

SessionCaptureCoordinator::Boundary::~Boundary()
{
	if (!m_committed)
		Rollback();
}

void SessionCaptureCoordinator::Boundary::Rollback() noexcept
{
	for (const auto id : m_created)
		m_owner.m_store.Release(id);
	m_created.clear();
	m_result.captures.clear();
}

CaptureBoundaryResult SessionCaptureCoordinator::Boundary::Capture(SessionGeneration generation,
																   const FramePipelineState& pipeline,
																   std::vector<PresentationSubmission> retained,
																   std::optional<FrameKey> after)
{
	m_retained = std::move(retained);
	if (!pipeline.paused)
	{
		m_result.result = SnapshotAdmission::Invalid;
		return std::move(m_result);
	}
	m_sources.reserve(m_retained.size() + 2);
	for (const auto& submitted : m_retained)
	{
		if (after && submitted.key <= *after)
			continue;
		if (!submitted.metadata || (submitted.metadata->submission && submitted.metadata->submission->layerCount &&
									(!m_owner.Supports() || !submitted.output)))
		{
			m_result.result = SnapshotAdmission::Unsupported;
			return std::move(m_result);
		}
		m_sources.push_back({submitted.metadata, {}, submitted.output});
	}
	for (const auto& prediction : {pipeline.begun, pipeline.waited})
	{
		if (!prediction)
			continue;
		const FrameKey key{generation, prediction->id};
		if (after && key <= *after)
			continue;
		if (std::any_of(m_sources.begin(), m_sources.end(),
						[&](const auto& source)
						{
							return source.metadata->key == key;
						}))
			continue;
		auto metadata = m_owner.m_history.Find(key);
		auto cancelled = m_owner.m_history.CancellationRecord(key);
		if (!metadata || !cancelled)
		{
			m_result.result = SnapshotAdmission::Invalid;
			return std::move(m_result);
		}
		m_sources.push_back({std::move(metadata), std::move(cancelled), {}});
	}
	std::sort(m_sources.begin(), m_sources.end(),
			  [](const auto& left, const auto& right)
			  {
				  return left.metadata->key < right.metadata->key;
			  });
	for (const auto& source : m_sources)
	{
		for (const auto id : m_owner.m_store.FindAll(source.metadata->key))
		{
			if (std::find(m_existing.begin(), m_existing.end(), id) == m_existing.end())
				m_existing.push_back(id);
		}
	}
	m_created.reserve(m_sources.size());
	try
	{
		for (const auto& source : m_sources)
		{
			if (source.output)
			{
				const auto promoted = m_owner.Promote(source.output, {});
				if (promoted != SnapshotAdmission::Accepted)
				{
					m_result.result = promoted;
					Rollback();
					return std::move(m_result);
				}
			}
			if (const auto existing = m_owner.m_store.Find(source.metadata->key))
			{
				m_result.captures.push_back(*existing);
				continue;
			}
			std::shared_ptr<FrameReservation> producer;
			if (source.metadata->stage == FrameStage::Issued || source.metadata->stage == FrameStage::Begun)
			{
				std::lock_guard lock(m_owner.m_mutex);
				if (!m_owner.m_pool)
				{
					m_result.result = SnapshotAdmission::Unsupported;
					Rollback();
					return std::move(m_result);
				}
				producer = m_owner.m_store.ProducerReservation(source.metadata->key);
				if (!producer)
					producer = m_owner.m_pool->Reserve(false);
				if (!producer)
				{
					m_result.result = SnapshotAdmission::Capacity;
					Rollback();
					return std::move(m_result);
				}
			}
			const auto capture =
				m_owner.m_store.Pin(source.metadata, source.cancelled, source.output, std::move(producer));
			if (capture.result != SnapshotAdmission::Accepted)
			{
				m_result.result = capture.result;
				Rollback();
				return std::move(m_result);
			}
			m_created.push_back(capture.id);
			m_result.captures.push_back(capture.id);
		}
	}
	catch (...)
	{
		Rollback();
		throw;
	}
	for (const auto id : m_existing)
	{
		if (std::find(m_result.captures.begin(), m_result.captures.end(), id) == m_result.captures.end())
			m_result.captures.push_back(id);
	}
	m_result.result = SnapshotAdmission::Accepted;
	m_committed = true;
	return std::move(m_result);
}
} // namespace simxr
