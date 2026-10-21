#pragma once

#include "graphics/RotationalSource.h"
#include "presentation/DisplayTimeline.h"

#include <cstddef>
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
	explicit PresentationController(std::size_t capacity = 8, FrameHistory* history = nullptr);
	void Start(SessionGeneration generation, const DisplayTimeline& timeline);
	// Reserve queue storage before committing frame state. The callback may throw
	// but must not reenter this controller. It runs before publication to readers.
	PresentationAdmission Submit(PresentationSubmission submission, const std::function<void()>& commit = {});
	PresentationAdmission SubmitWithMetadata(PresentationSubmission submission,
											 const std::function<std::shared_ptr<const FrameRecord>()>& commit);
	PresentationAdmission
	SubmitLiveClearWithMetadata(PresentationSubmission submission,
								const std::function<std::shared_ptr<const FrameRecord>()>& commit);
	std::optional<PresentationTick> Advance(RuntimeTime now, std::optional<SessionGeneration> generation = {});
	void Stop() noexcept;
	std::size_t PollRetired();
	PresentationSnapshot Snapshot() const;
	std::vector<PresentationSubmission> Retain() const;

  private:
	struct Entry
	{
		PresentationSubmission submission;
		bool retiring = false;
	};
	struct Observation
	{
		PresentationSubmission submission;
		CompletionStatus status = CompletionStatus::Pending;
	};
	const std::size_t m_capacity;
	FrameHistory* const m_history;
	mutable std::mutex m_mutex;
	bool m_running = false;
	std::uint64_t m_revision = 0;
	SessionGeneration m_generation;
	std::optional<DisplayTimeline> m_timeline;
	FrameId m_lastSubmitted;
	FrameId m_lastSelected;
	std::optional<PresentationSubmission> m_current;
	// The internal tick record contains metadata only. Snapshot/Advance attach
	// a CPU reference to the current output when returning it to a reader.
	std::optional<PresentationTick> m_latest;
	std::deque<Entry> m_entries;
	std::optional<PresentationSubmission> m_liveClear;
	std::uint64_t m_coalescedLiveClears = 0;
};
} // namespace simxr
