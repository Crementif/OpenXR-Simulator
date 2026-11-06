#pragma once

#include "platform/Clock.h"
#include "presentation/PresentationController.h"
#include "presentation/RotationalDisplay.h"

#include <atomic>
#include <condition_variable>
#include <cstddef>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <thread>
#include <vector>

namespace simxr
{
class VirtualDisplay
{
public:
	using HeadSampler = std::function<PoseSample(RuntimeTime)>;

	explicit VirtualDisplay(std::shared_ptr<Clock> clock = std::make_shared<SteadyClock>(), std::size_t capacity = 8,
							FrameHistory* history = nullptr, PresentationController::CompletionObserver completed = {});
	~VirtualDisplay();
	VirtualDisplay(const VirtualDisplay&) = delete;
	VirtualDisplay& operator=(const VirtualDisplay&) = delete;

	// Configure only while stopped. The sampler must not acquire session/lifecycle locks.
	void ConfigurePolicy(PresentationPolicy policy, HeadSampler sampleHead = {});
	PresentationPolicy Policy() const;

	void Start(SessionGeneration generation, const DisplayTimeline& timeline, ExecutionMode mode,
			   std::optional<HostTime> hostEpoch = {});
	void Stop() noexcept;

	// Pause joins wall-clock advancement without closing producer admission.
	bool Pause();
	bool Resume(HostTime hostAnchor, DisplayTick boundary);
	// Adopts the scheduler's timeline after a refresh-rate change.
	bool Retime(SessionGeneration generation, const DisplayTimeline& timeline);
	bool Paused() const;

	// Queue only; the existing display worker performs all image observation and
	// publication. Clock-domain validation belongs to the session coordinator.
	bool PublishExternalPrediction(SessionGeneration generation, std::uint64_t attachment, std::uint64_t sequence,
								   RuntimeTime target, Duration period);
	void ReleaseExternalPrediction(std::uint64_t attachment);

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
	void WaitForImage(std::optional<DisplayTick> after, std::chrono::steady_clock::time_point deadline) const;
	bool Faulted() const noexcept;
	std::size_t PollRetired();

private:
	std::optional<PresentationTick> AdvanceTick(RuntimeTime time, std::optional<SessionGeneration> generation = {},
												bool exact = false, const std::function<bool()>& current = {});
	void StopLocked() noexcept;
	void Run() noexcept;

	std::shared_ptr<Clock> m_clock;
	std::shared_ptr<Clock> m_waiter;
	PresentationController m_controller;
	RotationalDisplay m_reprojection;
	PresentationPolicy m_policy = PresentationPolicy::Repeat;
	HeadSampler m_sampleHead;
	std::mutex m_control;
	mutable std::mutex m_mutex;
	std::condition_variable m_changed;
	bool m_active = false;
	bool m_paused = false;
	ExecutionMode m_mode = ExecutionMode::RealTime;
	std::optional<DisplayTimeline> m_timeline;
	SessionGeneration m_generation;

	struct ExternalPrediction
	{
		std::uint64_t attachment = 0, sequence = 0, consumed = 0;
		RuntimeTime target;
		HostTime expires, targetHost;
	};

	std::optional<ExternalPrediction> m_external;
	HostTime m_hostEpoch;
	DisplayTick m_hostTick;
	std::thread m_worker;
	std::atomic<bool> m_faulted{false};
};
} // namespace simxr
