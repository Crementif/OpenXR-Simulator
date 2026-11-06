#pragma once

#include "graphics/RotationalSource.h"
#include "presentation/DisplayTimeline.h"

#include <chrono>
#include <condition_variable>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <vector>

namespace simxr
{
struct FrameRecord;
class FrameHistory;

struct PresentationSubmission
{
	FrameKey key;
	RuntimeTime targetTime;
	// A null output represents an explicit zero-layer clear, without GPU work.
	std::shared_ptr<const FrameOutput> output;
	std::shared_ptr<const FrameRecord> metadata;
	ReprojectionInput reprojection;
};

enum class PresentationAdmission
{
	Accepted,
	QueueFull,
	StaleGeneration,
	Closed,
	Invalid
};

enum class PresentationKind
{
	NewFrame,
	Repeat,
	Clear,
	Empty
};

enum class PresentationPolicy
{
	Repeat,
	Rotational
};

struct PresentationImage
{
	std::shared_ptr<const FrameOutput> output;
	std::optional<PoseSample> head;
	bool reprojected = false;
	XrResult result = XR_SUCCESS;
	// CPU enqueue cost and wall time until completion is observed this tick.
	// Completion latency includes queueing/polling; it is not GPU execution time.
	Duration submissionTime{};
	Duration completionLatency{};
};

struct PresentationTick
{
	DisplayTick tick;
	RuntimeTime time;
	PresentationKind kind = PresentationKind::Empty;
	std::optional<FrameKey> source;
	std::shared_ptr<const FrameOutput> output;
	std::uint64_t skippedTicks = 0;
	std::shared_ptr<const FrameRecord> metadata;
	ReprojectionInput reprojection;
	// First display tick selecting this image; unchanged when repeating it.
	std::optional<DisplayTick> imageTick;
	PresentationPolicy policy = PresentationPolicy::Repeat;
	std::optional<PoseSample> displayHead;
	bool reprojected = false;
	XrResult reprojectionResult = XR_SUCCESS;
	Duration reprojectionSubmissionTime{};
	Duration reprojectionCompletionLatency{};
};

struct PresentationSnapshot
{
	bool running = false;
	SessionGeneration generation;
	std::size_t capacity = 0;
	FrameId lastSubmitted;
	std::optional<PresentationTick> latest;
	std::vector<FrameKey> queued;
	std::vector<FrameKey> retiring;
	std::optional<FrameKey> liveClear;
	std::uint64_t coalescedLiveClears = 0;
};

class PresentationController
{
public:
	// The optional history must outlive this controller; tracing never takes its frame-record lock.
	// Completion observers run after publication, outside presentation locks.
	using CompletionObserver = std::function<void(FrameKey, const FramePrediction&, RuntimeTime)>;
	explicit PresentationController(std::size_t capacity = 8, FrameHistory* history = nullptr,
									CompletionObserver completed = {});

	void Start(SessionGeneration generation, const DisplayTimeline& timeline);
	// Adopts a timeline that differs only from a future tick on.
	void Retime(SessionGeneration generation, const DisplayTimeline& timeline);
	// Reserve queue storage before committing frame state. The callback may throw
	// but must not reenter this controller. It runs before publication to readers.
	PresentationAdmission Submit(PresentationSubmission submission, const std::function<void()>& commit = {});
	PresentationAdmission SubmitWithMetadata(PresentationSubmission submission,
											 const std::function<std::shared_ptr<const FrameRecord>()>& commit);
	PresentationAdmission
	SubmitLiveClearWithMetadata(PresentationSubmission submission,
								const std::function<std::shared_ptr<const FrameRecord>()>& commit);
	// The transform runs outside presentation locks and returns a completed image.
	// Stop/restart or another committed advance invalidates its pending publication.
	using ImageTransform = std::function<PresentationImage(const PresentationTick&)>;
	std::optional<PresentationTick> Advance(RuntimeTime now, std::optional<SessionGeneration> generation = {},
											const ImageTransform& transform = {});
	// Physical opportunities preserve their exact target time, including multiple
	// opportunities within one virtual interval. Tick remains a unique image identity.
	std::optional<PresentationTick> AdvanceExternal(RuntimeTime target, SessionGeneration generation,
													const ImageTransform& transform = {},
													const std::function<bool()>& current = {});
	void Stop() noexcept;

	std::size_t PollRetired();
	PresentationSnapshot Snapshot() const;
	std::vector<PresentationSubmission> Retain() const;
	// Returns once the selected image differs from `after`, presentation stops or the deadline passes.
	void WaitForImage(std::optional<DisplayTick> after, std::chrono::steady_clock::time_point deadline) const;

private:
	std::optional<PresentationTick> AdvanceAt(RuntimeTime now, std::optional<SessionGeneration> generation,
											  const ImageTransform& transform, bool exact,
											  const std::function<bool()>& current = {});

	struct Entry
	{
		PresentationSubmission submission;
		bool retiring = false;
		bool completionReported = false;
	};

	struct Observation
	{
		PresentationSubmission submission;
		CompletionStatus status = CompletionStatus::Pending;
		bool reportCompletion = false;
	};

	const std::size_t m_capacity;
	FrameHistory* const m_history;
	const CompletionObserver m_completed;
	mutable std::mutex m_mutex;
	mutable std::condition_variable m_published;
	bool m_running = false;
	std::uint64_t m_revision = 0;
	SessionGeneration m_generation;
	std::optional<DisplayTimeline> m_timeline;
	FrameId m_lastSubmitted;
	FrameId m_lastSelected;
	std::optional<PresentationSubmission> m_current;
	std::shared_ptr<const FrameOutput> m_displayOutput;
	// The internal tick record contains metadata only. Snapshot/Advance attach
	// a CPU reference to the current output when returning it to a reader.
	std::optional<PresentationTick> m_latest;
	std::deque<Entry> m_entries;
	std::optional<PresentationSubmission> m_liveClear;
	std::uint64_t m_coalescedLiveClears = 0;
};
} // namespace simxr
