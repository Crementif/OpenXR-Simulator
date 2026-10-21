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
	std::optional<HostTime> hostEpoch;
	std::uint32_t manualPermitLimit = 8;
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
};

class FrameScheduler
{
  public:
	explicit FrameScheduler(std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>());
	~FrameScheduler();
	void Start(const FrameSchedulerConfig& config);
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
	XrResult WaitFrame(FramePrediction& prediction, const std::function<void(const FramePrediction&)>& reserve = {});
	XrResult BeginFrame();
	XrResult EndFrame(FramePrediction& prediction);
	FramePipelineState Snapshot() const;

  private:
	std::shared_ptr<Clock> m_clock;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	FrameSchedulerConfig m_config;
	DisplayTimeline m_timeline;
	HostTime m_hostEpoch{};
	DisplayTick m_hostTick;
	FramePipelineState m_state;
	std::uint64_t m_revision = 0;
	std::uint64_t m_policyRevision = 0;
	std::uint64_t m_nextFrame = 1;
	DisplayTick m_nextAdmission;
};
} // namespace simxr
