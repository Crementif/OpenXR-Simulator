#pragma once

#include "platform/Clock.h"
#include "presentation/PresentationController.h"

#include <atomic>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>

namespace simxr
{
class VirtualDisplay
{
  public:
	explicit VirtualDisplay(std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>(), std::size_t capacity = 8,
							FrameHistory* history = nullptr);
	~VirtualDisplay();
	VirtualDisplay(const VirtualDisplay&) = delete;
	VirtualDisplay& operator=(const VirtualDisplay&) = delete;
	void Start(SessionGeneration generation, const DisplayTimeline& timeline, ExecutionMode mode,
			   std::optional<HostTime> hostEpoch = {});
	void Stop() noexcept;
	// Pause joins wall-clock advancement without closing producer admission.
	bool Pause();
	bool Resume(HostTime hostAnchor, DisplayTick boundary);
	bool Paused() const;
	DisplayTick NextTick() const;
	std::optional<PresentationTick> Step();
	PresentationAdmission Submit(PresentationSubmission submission, const std::function<void()>& commit = {});
	PresentationAdmission SubmitWithMetadata(PresentationSubmission submission,
											 const std::function<std::shared_ptr<const FrameRecord>()>& commit);
	PresentationAdmission
	SubmitLiveClearWithMetadata(PresentationSubmission submission,
								const std::function<std::shared_ptr<const FrameRecord>()>& commit);
	std::vector<PresentationSubmission> Retain() const;
	std::optional<PresentationTick> Advance(RuntimeTime time);
	void AdvanceFromFrame(SessionGeneration generation, RuntimeTime time) noexcept;
	PresentationSnapshot Snapshot() const;
	bool Faulted() const noexcept;
	std::size_t PollRetired();

  private:
	void StopLocked() noexcept;
	void Run() noexcept;
	std::shared_ptr<Clock> m_clock;
	std::shared_ptr<Clock> m_waiter;
	PresentationController m_controller;
	std::mutex m_control;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_active = false;
	bool m_paused = false;
	ExecutionMode m_mode = ExecutionMode::RealTime;
	std::optional<DisplayTimeline> m_timeline;
	HostTime m_hostEpoch;
	DisplayTick m_hostTick;
	std::thread m_worker;
	std::atomic<bool> m_faulted{false};
};
} // namespace simxr
