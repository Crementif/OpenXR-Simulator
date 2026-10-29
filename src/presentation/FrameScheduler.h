#pragma once

#include "platform/Clock.h"
#include "presentation/DisplayTimeline.h"

#include <condition_variable>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <openxr/openxr.h>
#include <optional>

namespace simxr
{
struct FrameSchedulerConfig
{
	RuntimeTime epoch{Duration{1000000000}};
	Duration period{11111111};
	ExecutionMode mode = ExecutionMode::RealTime;
	std::uint32_t cadence = 1;
	bool automaticCadence = false;
	std::optional<HostTime> hostEpoch;
	std::uint32_t manualPermitLimit = 8;
};

// Process-local correlation. HostTime is never serialized to another process.
struct FrameClockMapping
{
	SessionGeneration generation;
	std::uint64_t revision = 0;
	HostTime hostAnchor;
	RuntimeTime runtimeAnchor;
	RuntimeTime Map(HostTime time) const;
};

// Native OS instant corresponds to this bounded interval on the runtime timeline.
// Unlike FrameClockMapping, this value contains no process-local clock objects.
struct FrameNativeClockMapping
{
	SessionGeneration generation;
	std::uint64_t revision = 0;
	NativeClockDomain domain;
	std::int64_t nativeNanoseconds = 0;
	RuntimeTime runtimeBefore, runtimeAfter;
	std::int64_t resolutionNanoseconds = 0;
};

struct FramePipelineState
{
	bool running = false;
	bool waitPending = false;
	std::optional<FramePrediction> waited;
	std::optional<FramePrediction> begun;
	bool paused = false;
	std::uint32_t permits = 0;
	DisplayTick nextAdmission;
	std::uint32_t cadence = 1;
};

class FrameScheduler
{
public:
	explicit FrameScheduler(std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>());
	~FrameScheduler();

	void Start(const FrameSchedulerConfig& config, SessionGeneration generation = {});
	void ObserveCompletion(FrameKey key, const FramePrediction& prediction, RuntimeTime observed);
	void Stop();

	// Low-level admission controls. The caller owns command idempotence and must
	// reserve capture capacity before granting permits. Pause preserves admitted
	// work; neither pause nor resume is an XR session stop.
	FramePipelineState Pause();
	XrResult GrantPermits(std::uint32_t count = 1);
	std::uint32_t CancelPermits();
	// Align with a frozen display before issuing manual permits; admitted
	// predictions are immutable. Unused permits must first be cancelled.
	XrResult RebasePaused(DisplayTick boundary);
	// Maps virtual boundary to host anchor without changing the display epoch.
	// Repeating Resume while unpaused does not rebase again.
	XrResult Resume(HostTime anchor, DisplayTick boundary);
	// The reservation must not reenter the scheduler, acquire a session lock,
	// wait for GPU work, or perform transport operations. Prepare resources first.
	// An optional deferred publication lock is acquired after pacing, outside the
	// scheduler mutex, and remains owned on return for the caller's publication.
	XrResult WaitFrame(FramePrediction& prediction, const std::function<void(const FramePrediction&)>& reserve = {},
					   std::unique_lock<std::mutex>* publication = nullptr);
	XrResult BeginFrame();
	XrResult EndFrame(FramePrediction& prediction);

	FramePipelineState Snapshot() const;

	// Predictions are already mapped to this clock revision. One attachment owns
	// the latest-value slot; no transport or physical-runtime call runs here.
	// commitDisplay runs under this mutex, must not reenter the scheduler, and
	// may only queue CPU-owned display state. False leaves scheduler pacing unchanged.
	bool PublishExternalPrediction(SessionGeneration generation, std::uint64_t clockRevision, std::uint64_t attachment,
								   std::uint64_t sequence, RuntimeTime target, Duration period,
								   const std::function<bool()>& commitDisplay = {});
	void ReleaseExternalPrediction(std::uint64_t attachment);

	// No mapping while stopped, paused, or using deterministic/unpaced time.
	std::optional<FrameClockMapping> ClockMapping() const;
	std::optional<FrameNativeClockMapping> NativeClockMapping() const;
	std::optional<std::uint64_t> LiveInputClockRevision() const;
	// CPU-only admission, under the scheduler lock; must not reenter the scheduler.
	bool WithClockRevision(SessionGeneration generation, std::uint64_t revision, const std::function<void()>& admit);
	std::optional<RuntimeTime> MapHostTime(HostTime time, SessionGeneration generation, std::uint64_t revision) const;

private:
	std::shared_ptr<Clock> m_clock;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	FrameSchedulerConfig m_config;
	SessionGeneration m_generation;
	FrameId m_lastFeedback;
	unsigned m_slowFrames = 0;
	unsigned m_fastFrames = 0;
	DisplayTimeline m_timeline;
	HostTime m_hostEpoch;
	DisplayTick m_hostTick;
	FramePipelineState m_state;
	std::uint64_t m_revision = 0;
	std::uint64_t m_clockRevision = 0;
	std::uint64_t m_policyRevision = 0;
	std::uint64_t m_nextFrame = 1;
	DisplayTick m_nextAdmission;

	struct ExternalPrediction
	{
		std::uint64_t attachment = 0, sequence = 0, consumed = 0;
		RuntimeTime target;
		Duration period;
		HostTime expires;
	};

	std::optional<ExternalPrediction> m_external;
	RuntimeTime m_lastPrediction;
};
} // namespace simxr
