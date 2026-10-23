#pragma once

#include "graphics/FrameGpuTiming.h"
#include "graphics/LayerBuilder.h"
#include "presentation/CaptureTypes.h"
#include "presentation/FrameDiagnosticsJournal.h"
#include "presentation/FrameTypes.h"
#include "simulation/ActionRegistry.h"
#include "simulation/CommandTypes.h"
#include "simulation/HapticState.h"
#include "simulation/TrackingTypes.h"
#include "simulation/ViewConfiguration.h"

#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string_view>
#include <vector>

namespace simxr
{
enum class FrameStage
{
	Issued,
	Begun,
	Submitted,
	Discarded,
	Cancelled
};

enum class FrameOutputDisposition
{
	Unavailable,
	Submitted,
	Clear,
	Dropped
};
enum class FrameDropReason
{
	None,
	NativeCapacity,
	PresentationCapacity
};

struct FrameSubmissionState
{
	RuntimeTime displayTime;
	std::uint32_t layerCount = 0;
	XrEnvironmentBlendMode blendMode = XR_ENVIRONMENT_BLEND_MODE_OPAQUE;
	InputSnapshot input;
	ActionSyncSnapshot actions;
	std::shared_ptr<const AppliedCommandSnapshot> commands;
	std::shared_ptr<const FrameDiagnostics> diagnostics;
	std::optional<TrackingSnapshot> tracking;
	ViewSettings viewSettings;
	std::optional<ViewQuerySnapshot> viewQuery;
	std::vector<HapticOutput> haptics;
	std::shared_ptr<const FrameLayers> layers;
	// Submitted records CPU/native acceptance, never GPU completion.
	FrameOutputDisposition outputDisposition = FrameOutputDisposition::Unavailable;
	FrameDropReason dropReason = FrameDropReason::None;
	ViewLayout viewLayout = ViewLayout::Stereo;
	std::shared_ptr<const FrameGpuTiming> gpuTiming;
};

struct SequenceCaptureRecord
{
	SequenceCaptureIntent request;
	CaptureReservation reservation;
};
struct FrameRecord
{
	FrameKey key;
	FramePrediction prediction;
	FrameStage stage = FrameStage::Issued;
	std::optional<FrameSubmissionState> submission;
	std::optional<SequenceCaptureRecord> sequenceCapture;
	std::optional<Duration> CpuFrameTime() const noexcept;
};

struct FrameHistorySnapshot
{
	SessionGeneration generation;
	std::uint64_t evicted = 0;
	std::size_t capacity = 0;
	std::vector<std::shared_ptr<const FrameRecord>> frames;
	RuntimeTime origin;
};

class FrameHistory
{
  public:
	explicit FrameHistory(std::size_t capacity = 128, std::size_t diagnosticCapacity = 1024);
	// Allocation-free observation, including during teardown. Native/GPU callers
	// report only milestones they actually observe; no queue access or waits.
	FrameDiagnostic Trace(FrameTraceKind kind, FrameKey frame, RuntimeTime time, XrResult result = XR_SUCCESS,
						  FrameDiagnosticCategory category = FrameDiagnosticCategory::Timing, std::uint64_t related = 0,
						  std::string_view detail = {}) noexcept;
	FrameDiagnostics Diagnostics(std::uint64_t after = 0, std::size_t count = 128,
								 std::optional<FrameKey> frame = {}) const;
	// GPU observers retain only this bounded journal, never the session or its images.
	std::shared_ptr<FrameDiagnosticsJournal> DiagnosticsJournal() const
	{
		return m_diagnostics;
	}
	SessionGeneration StartGeneration(RuntimeTime origin = {});
	// Reserve copied diagnostic ownership before committing issue. The callback
	// must not reenter history. Failure rolls back insertion and eviction.
	void Issue(
		const FramePrediction& prediction,
		const std::function<void(std::shared_ptr<const FrameRecord>, std::shared_ptr<const FrameRecord>)>& reserve = {},
		std::optional<SequenceCaptureRecord> sequenceCapture = {});
	void Begin(FrameId frame, std::optional<FrameId> discarded = {});
	// Allocates the immutable record before admission. The callback may submit
	// native work at the permitted API boundary, but must not reenter this history.
	XrResult Submit(FrameId frame, FrameSubmissionState state, const std::function<XrResult()>& beforeCommit = {});
	void CancelPending() noexcept;
	std::shared_ptr<const FrameRecord> Find(FrameKey key) const;
	std::shared_ptr<const FrameRecord> CancellationRecord(FrameKey key) const;
	FrameHistorySnapshot Snapshot() const;

  private:
	struct Entry
	{
		std::shared_ptr<const FrameRecord> record;
		std::shared_ptr<const FrameRecord> cancelled;
	};
	using Records = std::deque<Entry>;
	Records::iterator Current(FrameId frame);
	void CancelPendingLocked() noexcept;
	const std::size_t m_capacity;
	mutable std::mutex m_mutex;
	SessionGeneration m_generation;
	RuntimeTime m_origin;
	std::uint64_t m_evicted = 0;
	Records m_records;
	std::shared_ptr<FrameDiagnosticsJournal> m_diagnostics;
};
} // namespace simxr
