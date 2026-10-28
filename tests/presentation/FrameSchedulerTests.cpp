#include "../TestSupport.h"
#include "presentation/FrameScheduler.h"

#ifdef _WIN32
#include "platform/windows/WindowsClock.h"
#endif

#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <type_traits>

using namespace simxr;
using namespace std::chrono_literals;

class FakeClock final : public Clock
{
  public:
	HostTime Now() const override
	{
		return HostTime{Duration{m_time.load()}};
	}

	void WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime) override
	{
		changed.wait_for(lock, 1ms);
	}

	void Advance(Duration duration)
	{
		m_time.fetch_add(duration.count());
	}

  private:
	std::atomic<std::int64_t> m_time{0};
};

#ifdef _WIN32
void WindowsExpiredDeadline()
{
	WindowsClock clock;
	std::condition_variable changed;
	std::mutex mutex;
	std::unique_lock lock(mutex);
	clock.WaitUntil(changed, lock, HostTime::min());
	Require(lock.owns_lock(), "Minimum Windows deadline released the caller lock");
	const auto expired = clock.Now();
	clock.WaitUntil(changed, lock, expired);
	Require(lock.owns_lock(), "Expired Windows deadline released the caller lock");
}
void WindowsPerformanceTime()
{
	std::int64_t frequency = 0;
	Require(TimeToPerformanceCounter(1000000000, frequency) == XR_SUCCESS && frequency > 0,
			"One second did not convert to a positive Windows counter period");
	XrTime second = 0;
	Require(PerformanceCounterToTime(frequency, second) == XR_SUCCESS && second == 1000000000,
			"Windows counter period did not convert back to one second");
	XrTime now = 0, roundTrip = 0;
	std::int64_t counter = 0;
	Require(ReadPerformanceTime(now) == XR_SUCCESS && now > 0 && TimeToPerformanceCounter(now, counter) == XR_SUCCESS &&
				PerformanceCounterToTime(counter, roundTrip) == XR_SUCCESS && roundTrip <= now &&
				now - roundTrip <= (1000000000 / frequency) + 1,
			"Current Windows time did not retain counter precision");
	for (const auto invalid : {std::int64_t{0}, std::int64_t{-1}, std::numeric_limits<std::int64_t>::min()})
	{
		counter = 77;
		second = 88;
		Require(PerformanceCounterToTime(invalid, second) == XR_ERROR_TIME_INVALID && second == 88 &&
					TimeToPerformanceCounter(invalid, counter) == XR_ERROR_TIME_INVALID && counter == 77,
				"Invalid Windows time changed output or was accepted");
	}
	if (frequency < 1000000000)
	{
		second = 88;
		Require(PerformanceCounterToTime(std::numeric_limits<std::int64_t>::max(), second) == XR_ERROR_TIME_INVALID &&
					second == 88,
				"Counter conversion overflow changed output or was accepted");
	}
	else if (frequency > 1000000000)
	{
		counter = 77;
		Require(TimeToPerformanceCounter(std::numeric_limits<std::int64_t>::max(), counter) == XR_ERROR_TIME_INVALID &&
					counter == 77,
				"Time conversion overflow changed output or was accepted");
	}
}

#endif

void HostElapsedOverflow()
{
	Require(HostTimeDifference(HostTime::max(), HostTime{}) == HostTime::duration::max() &&
				HostTimeDifference(HostTime::min(), HostTime{}) == HostTime::duration::min() &&
				HostTimeDifference(HostTime{}, HostTime::min() + HostTime::duration{1}) == HostTime::duration::max() &&
				HostTimeDifference(HostTime{}, HostTime::max()) == -HostTime::duration::max(),
			"Representable host interval boundary was rejected or changed");
	for (bool forward : {true, false})
	{
		auto clock = std::make_shared<FakeClock>();
		clock->Advance(
			Duration{forward ? std::numeric_limits<std::int64_t>::max() : std::numeric_limits<std::int64_t>::min()});
		FrameScheduler scheduler(clock);
		FrameSchedulerConfig config;
		config.hostEpoch = forward ? HostTime::min() : HostTime::max();
		scheduler.Start(config);
		FramePrediction frame{{77}};
		Require(scheduler.WaitFrame(frame) == XR_ERROR_RUNTIME_FAILURE && frame.id == FrameId{77} &&
					!scheduler.Snapshot().waitPending && !scheduler.Snapshot().waited &&
					scheduler.Snapshot().nextAdmission == DisplayTick{},
				"Host elapsed overflow fabricated admission or changed output");
		config.hostEpoch = clock->Now();
		scheduler.Start(config);
		Require(scheduler.WaitFrame(frame) == XR_SUCCESS && frame.id == FrameId{1},
				"Restart after elapsed overflow did not recover");
	}
}
void ManualAdmission()
{
	auto clock = std::make_shared<FakeClock>();
	FrameScheduler scheduler(clock);
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	config.epoch = {Duration{100}};
	config.period = 10ns;
	config.manualPermitLimit = 2;
	scheduler.Start(config);
	const auto boundary = scheduler.Pause();
	Require(boundary.running && boundary.paused && !boundary.waited && !boundary.begun &&
				boundary.nextAdmission == DisplayTick{},
			"Initial manual boundary");
	FramePrediction first;
	auto waiting = std::async(std::launch::async,
							  [&]
							  {
								  return scheduler.WaitFrame(first);
							  });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Manual wait did not enter");
	Require(waiting.wait_for(10ms) == std::future_status::timeout, "Paused scheduler admitted unpermitted frame");
	FramePrediction duplicate;
	Require(scheduler.WaitFrame(duplicate) == XR_ERROR_CALL_ORDER_INVALID, "Concurrent manual wait accepted");
	Require(scheduler.GrantPermits(0) == XR_ERROR_VALIDATION_FAILURE &&
				scheduler.GrantPermits(3) == XR_ERROR_LIMIT_REACHED,
			"Manual grant limit invalid");
	Require(scheduler.GrantPermits() == XR_SUCCESS, "Strict grant failed");
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_SUCCESS &&
				first.id == FrameId{1} && first.displayTick == DisplayTick{1} && scheduler.Snapshot().permits == 0,
			"Strict permit did not admit exactly one frame");
	Require(scheduler.GrantPermits(2) == XR_SUCCESS && scheduler.Pause().permits == 2,
			"Repeated pause revoked valid unused permits");
	Require(scheduler.CancelPermits() == 2 && scheduler.CancelPermits() == 0 &&
				scheduler.Snapshot().waited->id == first.id,
			"Cancellation changed already admitted frame");
	FramePrediction submitted;
	Require(scheduler.BeginFrame() == XR_SUCCESS && scheduler.EndFrame(submitted) == XR_SUCCESS &&
				submitted.id == first.id,
			"Pause blocked previously admitted work");
	Require(scheduler.GrantPermits() == XR_SUCCESS, "Retry reservation grant");
	FramePrediction untouched{{777}, {777}, {Duration{777}}, Duration{777}};
	Require(scheduler.WaitFrame(untouched,
								[](const auto&)
								{
									throw std::bad_alloc{};
								}) == XR_ERROR_OUT_OF_MEMORY &&
				scheduler.Snapshot().permits == 1 && scheduler.Snapshot().nextAdmission == DisplayTick{1} &&
				untouched.id == FrameId{777},
			"Failed reservation consumed strict permit or virtual identity");
	FramePrediction second, third, fourth{{99}};
	Require(scheduler.WaitFrame(second) == XR_SUCCESS && second.id == FrameId{2} &&
				scheduler.BeginFrame() == XR_SUCCESS,
			"Manual reservation retry changed identity");
	Require(scheduler.GrantPermits(2) == XR_SUCCESS && scheduler.WaitFrame(third) == XR_SUCCESS,
			"Overlapping manual admission");
	const auto overlap = scheduler.Pause();
	Require(overlap.begun->id == second.id && overlap.waited->id == third.id && overlap.permits == 1,
			"Manual pause omitted overlapping pipeline boundary");
	waiting = std::async(std::launch::async,
						 [&]
						 {
							 return scheduler.WaitFrame(fourth);
						 });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Manual overlap wait did not enter");
	Require(scheduler.CancelPermits() == 1 && scheduler.BeginFrame() == XR_FRAME_DISCARDED &&
				scheduler.EndFrame(submitted) == XR_SUCCESS && submitted.id == third.id,
			"Manual gate changed discard/order rules");
	Require(waiting.wait_for(10ms) == std::future_status::timeout, "Cancelled unused permit admitted work");
	scheduler.Stop();
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_ERROR_SESSION_NOT_RUNNING &&
				fourth.id == FrameId{99},
			"Stop did not cancel manual wait without fabricated frame");
	scheduler.Start(config);
	Require(!scheduler.Snapshot().paused && !scheduler.Snapshot().permits &&
				scheduler.GrantPermits() == XR_ERROR_CALL_ORDER_INVALID && scheduler.WaitFrame(first) == XR_SUCCESS &&
				first.id == FrameId{1},
			"Restart retained old manual policy/permits");
}

void PauseDuringPacingAndRebase()
{
	auto clock = std::make_shared<FakeClock>();
	FrameScheduler scheduler(clock);
	FrameSchedulerConfig config;
	config.epoch = {Duration{100}};
	config.period = 10ms;
	scheduler.Start(config);
	FramePrediction first, second, third, fourth, submitted;
	Require(scheduler.WaitFrame(first) == XR_SUCCESS && scheduler.BeginFrame() == XR_SUCCESS &&
				scheduler.EndFrame(submitted) == XR_SUCCESS,
			"Rebase first frame");
	auto waiting = std::async(std::launch::async,
							  [&]
							  {
								  return scheduler.WaitFrame(second);
							  });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Paced pause wait did not enter");
	const auto paused = scheduler.Pause();
	clock->Advance(1h);
	Require(paused.waitPending && waiting.wait_for(10ms) == std::future_status::timeout,
			"Pause failed to recheck admission inside paced wait");
	Require(scheduler.GrantPermits() == XR_SUCCESS && waiting.wait_for(1s) == std::future_status::ready &&
				waiting.get() == XR_SUCCESS && second.id == FrameId{2} && second.displayTick == DisplayTick{2},
			"Manual permit used elapsed inspection host time");
	Require(scheduler.BeginFrame() == XR_SUCCESS && scheduler.EndFrame(submitted) == XR_SUCCESS,
			"Manual paced frame completion");
	Require(scheduler.GrantPermits(2) == XR_SUCCESS, "Unused resume permits");
	const auto anchor = clock->Now();
	Require(scheduler.Resume(anchor, {10}) == XR_SUCCESS && !scheduler.Snapshot().paused &&
				!scheduler.Snapshot().permits && scheduler.Snapshot().nextAdmission == DisplayTick{10},
			"Resume did not rebase/preserve virtual boundary");
	Require(scheduler.Resume(anchor + 1h, {1000}) == XR_SUCCESS &&
				scheduler.Snapshot().nextAdmission == DisplayTick{10},
			"Repeated resume rebased again");
	Require(scheduler.WaitFrame(third) == XR_SUCCESS && third.id == FrameId{3} &&
				third.displayTick == DisplayTick{11} && third.displayTime > second.displayTime &&
				scheduler.BeginFrame() == XR_SUCCESS && scheduler.EndFrame(submitted) == XR_SUCCESS,
			"Resume decreased prediction or reset frame identity");
	waiting = std::async(std::launch::async,
						 [&]
						 {
							 return scheduler.WaitFrame(fourth);
						 });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Resumed paced wait did not enter");
	Require(waiting.wait_for(10ms) == std::future_status::timeout, "Resume emitted catch-up burst");
	clock->Advance(9ms);
	Require(waiting.wait_for(10ms) == std::future_status::timeout, "Resume lost shared host/tick anchor");
	clock->Advance(1ms);
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_SUCCESS &&
				fourth.displayTick == DisplayTick{12},
			"Resumed real-time deadline wrong");
}

void AlignPausedDisplayBoundary()
{
	auto clock = std::make_shared<FakeClock>();
	FrameScheduler scheduler(clock);
	FrameSchedulerConfig config;
	config.period = 10ms;
	scheduler.Start(config);
	FramePrediction first, second, strict, submitted;
	Require(scheduler.WaitFrame(first) == XR_SUCCESS && scheduler.BeginFrame() == XR_SUCCESS &&
				scheduler.EndFrame(submitted) == XR_SUCCESS,
			"Display alignment first frame");
	clock->Advance(10ms);
	Require(scheduler.WaitFrame(second) == XR_SUCCESS, "Display alignment early issued frame");
	// Display clock advances independently while the host keeps an issued frame.
	clock->Advance(1190ms);
	const DisplayTick frozenDisplayNext{121};
	const auto boundary = scheduler.Pause();
	Require(boundary.nextAdmission == DisplayTick{2} && boundary.waited->displayTick == DisplayTick{2},
			"Delayed host boundary was not preserved");
	Require(scheduler.RebasePaused(frozenDisplayNext) == XR_SUCCESS && scheduler.Snapshot().paused &&
				scheduler.Snapshot().nextAdmission == frozenDisplayNext &&
				scheduler.Snapshot().waited->displayTime == second.displayTime,
			"Paused rebase reopened admission or changed an issued prediction");
	Require(scheduler.RebasePaused({3}) == XR_SUCCESS && scheduler.Snapshot().nextAdmission == frozenDisplayNext,
			"Paused rebase regressed display boundary");
	Require(scheduler.GrantPermits() == XR_SUCCESS && scheduler.RebasePaused({130}) == XR_ERROR_CALL_ORDER_INVALID &&
				scheduler.Snapshot().nextAdmission == frozenDisplayNext,
			"Rebase invalidated an unused reserved permit");
	Require(scheduler.CancelPermits() == 1 && scheduler.RebasePaused({130}) == XR_SUCCESS &&
				scheduler.GrantPermits() == XR_SUCCESS,
			"Cancelled reservation did not permit boundary alignment");
	Require(scheduler.BeginFrame() == XR_SUCCESS && scheduler.WaitFrame(strict) == XR_SUCCESS &&
				strict.id == FrameId{3} && strict.displayTick == DisplayTick{131} &&
				strict.displayTime > second.displayTime && scheduler.Snapshot().paused,
			"Strict frame predicted behind independently frozen display");
	Require(scheduler.EndFrame(submitted) == XR_SUCCESS && submitted.displayTime == second.displayTime,
			"Alignment rewrote already begun frame target");
}

void PublicationContention()
{
	FrameScheduler scheduler;
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Unpaced;
	scheduler.Start(config);
	std::mutex publication;
	std::unique_lock held(publication);
	auto waiting = std::async(std::launch::async,
							  [&]
							  {
								  std::unique_lock boundary(publication, std::defer_lock);
								  FramePrediction frame;
								  return scheduler.WaitFrame(frame, {}, &boundary);
							  });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Publication waiter not admitted");
	// Pausing must remain possible while publication is blocked, and that pause
	// must be rechecked before the candidate consumes an application frame.
	scheduler.Pause();
	held.unlock();
	Require(waiting.wait_for(5ms) == std::future_status::timeout && !scheduler.Snapshot().waited,
			"Publication contention bypassed a concurrent pause");
	Eventually(
		[&]
		{
			if (!publication.try_lock())
				return false;
			publication.unlock();
			return true;
		},
		"Paused pacing retained the publication lock");
	Require(scheduler.GrantPermits() == XR_SUCCESS && waiting.wait_for(1s) == std::future_status::ready &&
				waiting.get() == XR_SUCCESS,
			"Publication waiter did not resume after a permit");
}

void PauseReservationBoundaryAndLimits()
{
	auto clock = std::make_shared<FakeClock>();
	FrameScheduler scheduler(clock);
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Unpaced;
	config.period = 10ns;
	scheduler.Start(config);
	scheduler.Pause();
	Require(scheduler.GrantPermits() == XR_SUCCESS, "Boundary grant");
	std::promise<void> entered, release;
	auto released = release.get_future().share();
	FramePrediction frame;
	auto waiting = std::async(std::launch::async,
							  [&]
							  {
								  return scheduler.WaitFrame(frame,
															 [&](const auto&)
															 {
																 entered.set_value();
																 released.wait();
															 });
							  });
	Require(entered.get_future().wait_for(1s) == std::future_status::ready, "Reservation boundary not entered");
	auto pausing = std::async(std::launch::async,
							  [&]
							  {
								  return scheduler.Pause();
							  });
	Require(pausing.wait_for(5ms) == std::future_status::timeout, "Pause raced partially committed issue");
	release.set_value();
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_SUCCESS &&
				pausing.wait_for(1s) == std::future_status::ready && pausing.get().waited->id == frame.id,
			"Pause boundary omitted committed reservation");
	Require(scheduler.GrantPermits() == XR_SUCCESS &&
				scheduler.Resume(clock->Now(), {std::numeric_limits<std::uint64_t>::max()}) ==
					XR_ERROR_RUNTIME_FAILURE &&
				scheduler.Snapshot().paused && scheduler.Snapshot().permits == 1,
			"Unrepresentable resume partially changed policy");
	FramePrediction submitted;
	Require(scheduler.BeginFrame() == XR_SUCCESS && scheduler.EndFrame(submitted) == XR_SUCCESS &&
				scheduler.Resume(clock->Now(), {33}) == XR_SUCCESS && scheduler.WaitFrame(frame) == XR_SUCCESS &&
				frame.displayTick == DisplayTick{34},
			"Unpaced resume unexpectedly depended on host time");
	scheduler.Stop();

	auto nearMinimum = std::make_shared<FakeClock>();
	const auto anchor = HostTime::min() + 100ns;
	nearMinimum->Advance(std::chrono::duration_cast<Duration>(anchor.time_since_epoch()));
	FrameScheduler lower(nearMinimum);
	config.mode = ExecutionMode::RealTime;
	config.hostEpoch = anchor;
	lower.Start(config);
	Require(lower.WaitFrame(frame) == XR_SUCCESS && lower.BeginFrame() == XR_SUCCESS &&
				lower.EndFrame(submitted) == XR_SUCCESS,
			"Lower host limit initial frame");
	lower.Pause();
	Require(lower.Resume(anchor, {100}) == XR_SUCCESS && lower.WaitFrame(frame) == XR_SUCCESS &&
				frame.displayTick == DisplayTick{101},
			"Rebase subtracted large virtual tick from host anchor and underflowed");
}

void LargeCadencePeriod()
{
	auto clock = std::make_shared<FakeClock>();
	FrameScheduler scheduler(clock);
	FrameSchedulerConfig config;
	config.epoch = {1ns};
	config.period = Duration{std::numeric_limits<std::int64_t>::max() / 2 + 1};
	config.automaticCadence = true;
	scheduler.Start(config, {7});
	FramePrediction prediction, submitted;
	Require(scheduler.WaitFrame(prediction) == XR_SUCCESS && scheduler.BeginFrame() == XR_SUCCESS &&
				scheduler.EndFrame(submitted) == XR_SUCCESS && prediction.displayPeriod == config.period,
			"Representable large-period frame was rejected");
	scheduler.ObserveCompletion({{7}, prediction.id}, prediction, {prediction.displayTime.value + 1ns});
	Require(scheduler.Snapshot().cadence == 1, "Single large-period completion changed cadence");
	clock->Advance(config.period);
	FramePrediction untouched{{77}};
	Require(scheduler.WaitFrame(untouched) == XR_ERROR_RUNTIME_FAILURE && untouched.id == FrameId{77} &&
				!scheduler.Snapshot().waitPending && !scheduler.Snapshot().waited &&
				scheduler.Snapshot().nextAdmission == DisplayTick{1},
			"Exhausted large-period timeline fabricated admission or changed output");
}

void AutomaticCadence()
{
	auto clock = std::make_shared<FakeClock>();
	FrameScheduler scheduler(clock);
	FrameSchedulerConfig config;
	config.period = 10ms;
	config.automaticCadence = true;
	scheduler.Start(config, {7});
	const auto frame = [&](bool slow)
	{
		FramePrediction prediction;
		auto waiting = std::async(std::launch::async,
								  [&]
								  {
									  return scheduler.WaitFrame(prediction);
								  });
		for (int attempts = 0; attempts < 500 && waiting.wait_for(1ms) != std::future_status::ready; ++attempts)
			clock->Advance(1ms);
		if (waiting.wait_for(0ms) != std::future_status::ready)
			scheduler.Stop();
		Require(waiting.get() == XR_SUCCESS, "Adaptive wait");
		Require(scheduler.BeginFrame() == XR_SUCCESS, "Adaptive begin");
		FramePrediction submitted;
		Require(scheduler.EndFrame(submitted) == XR_SUCCESS, "Adaptive end");
		const RuntimeTime observed{prediction.displayTime.value - prediction.displayPeriod + (slow ? 20ms : 10ms)};
		scheduler.ObserveCompletion({{7}, prediction.id}, prediction, observed);
		clock->Advance(30ms);
		return prediction;
	};
	frame(true);
	frame(true);
	Require(scheduler.Snapshot().cadence == 1, "Cadence reacted before hysteresis threshold");
	const auto old = frame(true);
	Require(scheduler.Snapshot().cadence == 2 && old.displayPeriod == 10ms,
			"Slow completion did not lower cadence or changed an issued prediction");
	for (int i = 0; i < 29; ++i)
	{
		Require(frame(false).displayPeriod == 20ms, "Half-rate prediction changed before recovery");
		if (i == 14)
			frame(true); // Second-tick observation is neutral, not evidence that recovery was lost.
	}
	Require(scheduler.Snapshot().cadence == 2, "Recovery hysteresis ended early");
	frame(false);
	Require(scheduler.Snapshot().cadence == 1 && frame(false).displayPeriod == 10ms,
			"Fast completions did not restore full cadence");
	frame(true);
	const auto pending = frame(true);
	scheduler.Pause();
	scheduler.ObserveCompletion({{7}, pending.id}, pending, {pending.displayTime.value + 100ms});
	Require(scheduler.Resume(clock->Now(), scheduler.Snapshot().nextAdmission) == XR_SUCCESS, "Adaptive resume");
	frame(true);
	Require(scheduler.Snapshot().cadence == 1, "Pause retained stale hysteresis");
	scheduler.Stop();
	scheduler.Start(config, {8});
	scheduler.ObserveCompletion({{7}, pending.id}, pending, {pending.displayTime.value + 100ms});
	Require(scheduler.Snapshot().cadence == 1, "Old generation changed new cadence");
	for (auto mode : {ExecutionMode::Deterministic, ExecutionMode::Unpaced})
	{
		scheduler.Stop();
		config.mode = mode;
		scheduler.Start(config, {7});
		for (int i = 0; i < 4; ++i)
			frame(true);
		Require(scheduler.Snapshot().cadence == 1, "Virtual execution adapted to completion timing");
	}
}

void ExternalClockMapping()
{
	auto clock = std::make_shared<FakeClock>();
	FrameScheduler scheduler(clock);
	FrameSchedulerConfig config;
	config.period = 10ms;
	const SessionGeneration generation{7};
	Require(!scheduler.ClockMapping(), "Stopped scheduler cannot map external time");
	scheduler.Start(config, generation);
	const auto initial = *scheduler.ClockMapping();
	Require(!scheduler.NativeClockMapping(), "Synthetic clocks cannot invent OS correlation");
	Require(initial.generation == generation && initial.revision &&
				initial.Map(clock->Now() + 5ms).value == config.epoch.value + 5ms,
			"Map host prediction into the live runtime epoch");
	FramePrediction waited;
	Require(scheduler.WaitFrame(waited) == XR_SUCCESS, "Issue immutable prediction before pause");
	scheduler.Pause();
	Require(!scheduler.ClockMapping() && !scheduler.MapHostTime(clock->Now(), generation, initial.revision),
			"Pause invalidates external time mapping");
	clock->Advance(5s);
	Require(scheduler.Resume(clock->Now(), {20}) == XR_SUCCESS, "Resume with rebased virtual time");
	const auto resumed = *scheduler.ClockMapping();
	Require(resumed.revision != initial.revision && resumed.runtimeAnchor.value == config.epoch.value + 200ms &&
				resumed.Map(clock->Now() + 5ms).value == config.epoch.value + 205ms,
			"Resume maps new physical time into frozen virtual timeline");
	Require(!scheduler.MapHostTime(clock->Now(), generation, initial.revision) &&
				!scheduler.MapHostTime(clock->Now(), {8}, resumed.revision),
			"Reject old clock revision or run");
	Require(scheduler.Snapshot().waited->displayTime == waited.displayTime,
			"Clock remapping must not rewrite an issued prediction");
	Require(scheduler.Resume(clock->Now() + 1s, {30}) == XR_SUCCESS &&
				scheduler.ClockMapping()->revision == resumed.revision,
			"Duplicate resume keeps mapping identity");
	Require(scheduler.MapHostTime(clock->Now() - 1ms, generation, resumed.revision)->value ==
				config.epoch.value + 199ms,
			"Historical host query retains signed offset");
	for (const auto host : {HostTime::min(), HostTime::max()})
	{
		FrameClockMapping bounded{generation, 1, HostTime{}, {Duration{std::numeric_limits<std::int64_t>::max()}}};
		bool rejected = false;
		try
		{
			bounded.Map(host);
		}
		catch (const std::overflow_error&)
		{
			rejected = true;
		}
		Require(rejected, "Unrepresentable mapping cannot overflow or fabricate a valid timestamp");
	}
	FrameScheduler native;
	native.Start(config, {12});
	const auto correlated = native.NativeClockMapping();
	Require(correlated && correlated->generation == SessionGeneration{12} && correlated->revision &&
				correlated->runtimeBefore <= correlated->runtimeAfter && correlated->nativeNanoseconds > 0,
			"Native mapping retains run identity and measured uncertainty");
	native.Pause();
	Require(!native.NativeClockMapping(), "Paused scheduler cannot export a stale native mapping");
	scheduler.Stop();
	Require(!scheduler.ClockMapping(), "Stop retires live mapping");
	scheduler.Start(config, {8});
	Require(scheduler.ClockMapping()->revision != resumed.revision, "Restart receives new clock identity");
	for (const auto mode : {ExecutionMode::Deterministic, ExecutionMode::Unpaced})
	{
		config.mode = mode;
		scheduler.Start(config, {9});
		Require(!scheduler.ClockMapping(), "Non-real-time execution cannot advertise physical clock mapping");
	}
}

static void ExternalPredictions()
{
	auto clock = std::make_shared<FakeClock>();
	FrameScheduler scheduler(clock);
	FrameSchedulerConfig config;
	config.period = 10ms;
	scheduler.Start(config, {7});
	auto revision = scheduler.ClockMapping()->revision;
	const auto publish = [&](std::uint64_t sequence, Duration offset, std::uint64_t owner = 11)
	{
		return scheduler.PublishExternalPrediction({7}, revision, owner, sequence, {config.epoch.value + offset}, 10ms);
	};
	FramePrediction first, second, ended;
	Require(scheduler.WaitFrame(first) == XR_SUCCESS && scheduler.BeginFrame() == XR_SUCCESS,
			"Issue virtual prediction before physical attachment");
	Require(!scheduler.PublishExternalPrediction({7}, revision, 11, 1, {config.epoch.value + 15ms}, 10ms,
												 []
												 {
													 return false;
												 }),
			"Display rejection cannot commit scheduler pacing");
	Require(publish(1, 15ms), "Accept physical prediction in current clock domain");
	Require(scheduler.WaitFrame(second,
								[](const auto&)
								{
									throw std::bad_alloc{};
								}) == XR_ERROR_OUT_OF_MEMORY &&
				!scheduler.Snapshot().waited && !scheduler.Snapshot().waitPending,
			"Failed reservation does not consume the external pulse or poison admission");
	Require(scheduler.WaitFrame(second) == XR_SUCCESS && second.displayTime.value == config.epoch.value + 15ms,
			"Physical target is preserved instead of rounded to a virtual refresh");
	Require(scheduler.EndFrame(ended) == XR_SUCCESS && ended.displayTime == first.displayTime,
			"Attachment cannot rewrite a begun prediction");
	Require(!publish(1, 16ms) && !publish(2, 14ms) && !publish(2, 20ms, 12),
			"Reject duplicate, reversed and foreign physical pulses");
	Require(!scheduler.PublishExternalPrediction({8}, revision, 11, 2, {config.epoch.value + 20ms}, 10ms) &&
				!scheduler.PublishExternalPrediction({7}, revision + 1, 11, 2, {config.epoch.value + 20ms}, 10ms) &&
				!publish(2, 100ms),
			"Reject stale generation, stale clock and unbounded prediction horizon");
	scheduler.ReleaseExternalPrediction(12);
	Require(publish(2, 20ms) && publish(3, 25ms), "Newest pulse replaces unconsumed physical prediction");
	Require(scheduler.Snapshot().waited->displayTime == second.displayTime,
			"New physical pulses cannot rewrite a waited prediction");
	Require(scheduler.BeginFrame() == XR_SUCCESS && scheduler.EndFrame(ended) == XR_SUCCESS,
			"Retire waited physical frame");
	Require(scheduler.WaitFrame(first) == XR_SUCCESS && first.displayTime.value == config.epoch.value + 25ms,
			"Skip superseded pulses without a catch-up burst");
	Require(scheduler.BeginFrame() == XR_SUCCESS && scheduler.EndFrame(ended) == XR_SUCCESS,
			"Retire newest physical frame");
	auto waiting = std::async(std::launch::async,
							  [&]
							  {
								  return scheduler.WaitFrame(second);
							  });
	const auto cleanup = std::shared_ptr<void>(nullptr,
											   [&](void*)
											   {
												   scheduler.Stop();
											   });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Wait for another physical pulse");
	Require(waiting.wait_for(10ms) == std::future_status::timeout, "Cannot issue one pulse twice");
	clock->Advance(40ms);
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_SUCCESS &&
				second.displayTime.value == config.epoch.value + 60ms && second.displayPeriod == 10ms,
			"Expired provider resumes late virtual admission without an extra tick wait");
	Require(scheduler.BeginFrame() == XR_SUCCESS && scheduler.EndFrame(ended) == XR_SUCCESS,
			"Retire recovered virtual frame");
	waiting = std::async(std::launch::async,
						 [&]
						 {
							 return scheduler.WaitFrame(first);
						 });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Recovered virtual frame is paced");
	clock->Advance(19ms);
	Require(waiting.wait_for(10ms) == std::future_status::timeout, "Provider expiry allowed a catch-up burst");
	clock->Advance(1ms);
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_SUCCESS &&
				first.displayTime.value == config.epoch.value + 70ms,
			"Provider expiry preserves the next virtual display boundary");
	scheduler.Pause();
	Require(!publish(4, 60ms), "Paused admission rejects physical predictions");
	scheduler.Stop();

	config.cadence = 2;
	config.hostEpoch = clock->Now();
	scheduler.Start(config, {7});
	revision = scheduler.ClockMapping()->revision;
	Require(publish(10, 10ms) && scheduler.WaitFrame(first) == XR_SUCCESS && first.displayPeriod == 20ms,
			"Physical period drives half-rate application period");
	Require(scheduler.BeginFrame() == XR_SUCCESS && scheduler.EndFrame(ended) == XR_SUCCESS,
			"Retire first half-rate frame");
	Require(publish(11, 20ms), "Receive intervening physical pulse");
	waiting = std::async(std::launch::async,
						 [&]
						 {
							 return scheduler.WaitFrame(second);
						 });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Half-rate wait entered");
	Require(waiting.wait_for(10ms) == std::future_status::timeout, "Half rate skips intervening physical pulse");
	Require(publish(12, 30ms), "Receive next half-rate target");
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_SUCCESS &&
				second.displayTime.value == config.epoch.value + 30ms,
			"Half-rate physical pulse wakes admission");
	scheduler.ReleaseExternalPrediction(11);
	Require(scheduler.Snapshot().waited->displayTime == second.displayTime,
			"Detach preserves issued physical prediction");
	Require(scheduler.BeginFrame() == XR_SUCCESS && scheduler.EndFrame(ended) == XR_SUCCESS && publish(20, 40ms) &&
				scheduler.WaitFrame(first) == XR_SUCCESS && scheduler.BeginFrame() == XR_SUCCESS &&
				scheduler.EndFrame(ended) == XR_SUCCESS,
			"Reattach for provider-wait shutdown");
	waiting = std::async(std::launch::async,
						 [&]
						 {
							 return scheduler.WaitFrame(second);
						 });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Provider-wait shutdown entered");
	Require(waiting.wait_for(10ms) == std::future_status::timeout, "Provider wait remains blocked without a pulse");
	scheduler.Stop();
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_ERROR_SESSION_NOT_RUNNING,
			"Shutdown wakes external pacing without a provider response");
	config.mode = ExecutionMode::Deterministic;
	scheduler.Start(config, {7});
	Require(!publish(13, 40ms), "Deterministic execution cannot attach wall-clock pacing");
	scheduler.Stop();
}

int main()
{
	ExternalPredictions();
	ExternalClockMapping();
#ifdef _WIN32
	WindowsExpiredDeadline();
	WindowsPerformanceTime();
#endif
	LargeCadencePeriod();
	AutomaticCadence();
	HostElapsedOverflow();
	ManualAdmission();
	PauseDuringPacingAndRebase();
	AlignPausedDisplayBoundary();
	PublicationContention();
	PauseReservationBoundaryAndLimits();
	static_assert(!std::is_convertible_v<FrameId, DisplayTick>);
	static_assert(!std::is_convertible_v<RuntimeTime, SimulationTime>);
	DisplayTimeline timeline({Duration{7000000000000000000LL}}, 10ns);
	Require(timeline.TimeAt({123}).value.count() == 7000000000000001230LL, "Large timestamp precision");
	Require(timeline.TickAtOrAfter(timeline.TimeAt({123})) == DisplayTick{123}, "Exact tick");
	Require(timeline.TickAtOrAfter({timeline.TimeAt({123}).value + 1ns}) == DisplayTick{124}, "Ceiling tick");
	bool overflow = false;
	try
	{
		timeline.TimeAt({std::numeric_limits<std::uint64_t>::max()});
	}
	catch (const std::overflow_error&)
	{
		overflow = true;
	}
	Require(overflow, "Overflow must be explicit");

	auto clock = std::make_shared<FakeClock>();
	FrameScheduler scheduler(clock);
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	config.period = 10ns;
	FramePrediction first, second, submitted;
	Require(scheduler.WaitFrame(first) == XR_ERROR_SESSION_NOT_RUNNING, "Wait before session");
	scheduler.Start(config);
	Require(scheduler.BeginFrame() == XR_ERROR_CALL_ORDER_INVALID, "Begin without wait");
	Require(scheduler.EndFrame(submitted) == XR_ERROR_CALL_ORDER_INVALID, "End without begin");
	Require(scheduler.WaitFrame(first) == XR_SUCCESS, "Initial wait");
	Require(first.id == FrameId{1} && first.displayTime.value == config.epoch.value + 10ns, "Deterministic epoch");
	auto waiting = std::async(std::launch::async,
							  [&]
							  {
								  return scheduler.WaitFrame(second);
							  });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Second wait not entered");
	Require(waiting.wait_for(20ms) == std::future_status::timeout, "Second wait passed prior begin");
	Require(scheduler.BeginFrame() == XR_SUCCESS, "Begin first frame");
	Require(waiting.wait_for(1s) == std::future_status::ready, "Wait requires previous end");
	Require(waiting.get() == XR_SUCCESS, "Overlapping wait failed");
	Require(second.id == FrameId{2}, "Frame identity");
	Require(scheduler.EndFrame(submitted) == XR_SUCCESS && submitted.id == first.id, "End consumed wrong prediction");
	Require(scheduler.BeginFrame() == XR_SUCCESS, "Begin second");
	Require(scheduler.WaitFrame(first) == XR_SUCCESS, "Third wait");
	Require(scheduler.BeginFrame() == XR_FRAME_DISCARDED, "Missing discarded result");
	Require(scheduler.EndFrame(submitted) == XR_SUCCESS && submitted.id == FrameId{3},
			"Discarded frame replaced incorrectly");
	Require(scheduler.EndFrame(submitted) == XR_ERROR_CALL_ORDER_INVALID, "Double end accepted");

	Require(scheduler.WaitFrame(first) == XR_SUCCESS, "Pending-stop first wait");
	waiting = std::async(std::launch::async,
						 [&]
						 {
							 return scheduler.WaitFrame(second);
						 });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Stop wait not entered");
	scheduler.Stop();
	Require(waiting.wait_for(1s) == std::future_status::ready, "Stop did not wake blocked wait");
	Require(waiting.get() == XR_ERROR_SESSION_NOT_RUNNING, "Stop fabricated success");
	scheduler.Start(config);
	Require(scheduler.WaitFrame(first) == XR_SUCCESS && first.id == FrameId{1}, "Restart did not reset state");
	Require(scheduler.BeginFrame() == XR_SUCCESS, "Restart begin");
	scheduler.Stop();

	config.mode = ExecutionMode::RealTime;
	config.period = 10ms;
	scheduler.Start(config);
	Require(scheduler.WaitFrame(first) == XR_SUCCESS, "Real time initial wait");
	Require(scheduler.BeginFrame() == XR_SUCCESS, "Real time begin");
	waiting = std::async(std::launch::async,
						 [&]
						 {
							 return scheduler.WaitFrame(second);
						 });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Pacing not entered");
	Require(scheduler.EndFrame(submitted) == XR_SUCCESS, "End blocked by pacing");
	Require(waiting.wait_for(20ms) == std::future_status::timeout, "Real time ignores clock");
	clock->Advance(10ms);
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_SUCCESS,
			"Clock did not advance wait");
	Require(second.displayTime.value == config.epoch.value + 20ms, "Wrong prediction after pacing");
	Require(scheduler.BeginFrame() == XR_SUCCESS, "Paced begin");
	clock->Advance(107ms);
	waiting = std::async(std::launch::async,
						 [&]
						 {
							 return scheduler.WaitFrame(first);
						 });
	if (waiting.wait_for(1s) != std::future_status::ready)
	{
		scheduler.Stop();
		Require(false, "A late application waited for another display tick");
	}
	Require(waiting.get() == XR_SUCCESS, "Late admission failed");
	Require(first.displayTime.value == config.epoch.value + 130ms, "Stall prediction lost phase");
	Require(first.displayTime > second.displayTime, "Prediction regressed");
	Require(scheduler.BeginFrame() == XR_FRAME_DISCARDED, "Late frame did not replace its unfinished predecessor");
	waiting = std::async(std::launch::async,
						 [&]
						 {
							 return scheduler.WaitFrame(second);
						 });
	Eventually(
		[&]
		{
			return scheduler.Snapshot().waitPending;
		},
		"Post-stall pacing not entered");
	Require(waiting.wait_for(20ms) == std::future_status::timeout, "Late admission allowed a catch-up burst");
	clock->Advance(12ms);
	Require(waiting.wait_for(20ms) == std::future_status::timeout, "Post-stall pacing returned before its boundary");
	clock->Advance(1ms);
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_SUCCESS &&
				second.displayTime.value == config.epoch.value + 140ms,
			"Post-stall admission lost display phase");
	scheduler.Stop();

	config.mode = ExecutionMode::Deterministic;
	config.cadence = 2;
	scheduler.Start(config);
	Require(scheduler.WaitFrame(first) == XR_SUCCESS, "Half-rate wait");
	Require(first.displayPeriod == 20ms && first.displayTime.value == config.epoch.value + 20ms, "Half-rate cadence");
	scheduler.Stop();
	scheduler.Start(config);
	FramePrediction untouched{{777}, {777}, {Duration{777}}, Duration{777}};
	Require(scheduler.WaitFrame(untouched,
								[](const auto&)
								{
									throw std::bad_alloc{};
								}) == XR_ERROR_OUT_OF_MEMORY,
			"Failed reservation did not report allocation failure");
	Require(!scheduler.Snapshot().waited && !scheduler.Snapshot().waitPending && untouched.id == FrameId{777},
			"Failed reservation consumed frame admission");
	Require(scheduler.WaitFrame(first) == XR_SUCCESS && first.id == FrameId{1} && first.displayTick == DisplayTick{2},
			"Reservation retry advanced identity or virtual time");
	scheduler.Stop();
	config.mode = ExecutionMode::RealTime;
	config.cadence = 1;
	config.period = 10ns;
	config.hostEpoch = HostTime::max() - 5ns;
	auto nearLimit = std::make_shared<FakeClock>();
	nearLimit->Advance(std::chrono::duration_cast<Duration>(config.hostEpoch->time_since_epoch()));
	FrameScheduler bounded(nearLimit);
	bounded.Start(config);
	Require(bounded.WaitFrame(first) == XR_SUCCESS && bounded.BeginFrame() == XR_SUCCESS,
			"Host time limit first admission failed");
	Require(bounded.WaitFrame(second) == XR_ERROR_RUNTIME_FAILURE && !bounded.Snapshot().waited &&
				!bounded.Snapshot().waitPending,
			"Host deadline overflow fabricated a frame or poisoned admission");
	bounded.Stop();
	std::cout << "Frame scheduler tests passed\n";
}
