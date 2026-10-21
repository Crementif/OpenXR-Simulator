#include "../TestSupport.h"
#include "presentation/FrameScheduler.h"
#include "presentation/VirtualDisplay.h"
#include "runtime/Runtime.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>

using namespace simxr;
using namespace std::chrono_literals;

namespace
{
class TestClock final : public Clock
{
  public:
	HostTime Now() const override
	{
		if (fail.load())
			throw std::runtime_error("Clock failed");
		return HostTime{Duration{time.load()}};
	}
	void WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime) override
	{
		changed.wait_for(lock, 1ms);
	}
	std::atomic<std::int64_t> time{0};
	std::atomic<bool> fail{false};
};

class Output final : public FrameOutput
{
  public:
	CompletionStatus PollCompletion() const noexcept override
	{
		if (display)
			display->Snapshot();
		return state.load();
	}
	std::atomic<CompletionStatus> state{CompletionStatus::Pending};
	VirtualDisplay* display = nullptr;
};

void Worker()
{
	auto clock = std::make_shared<TestClock>();
	VirtualDisplay display(clock);
	display.Start({1}, {{Duration{100}}, 10ns}, ExecutionMode::RealTime, HostTime{});
	Eventually([&] { return display.Snapshot().latest.has_value(); }, "Worker did not run without XR calls");
	auto image = std::make_shared<Output>();
	image->display = &display;
	Require(display.Submit({{{1}, {1}}, {Duration{110}}, image}) == PresentationAdmission::Accepted,
			"Worker submission failed");
	clock->time.store(10);
	Eventually([&] { return display.Snapshot().latest->tick == DisplayTick{1}; }, "Worker did not advance clock");
	Require(display.Snapshot().latest->kind == PresentationKind::Empty, "Pending GPU output was displayed");
	image->state.store(CompletionStatus::Complete);
	clock->time.store(20);
	Eventually([&] { return display.Snapshot().latest->tick == DisplayTick{2}; }, "Worker did not observe completion");
	Require(display.Snapshot().latest->source == FrameKey{{1}, {1}}, "GPU completion was not independently selected");
	const auto retained = *display.Snapshot().latest;
	Require(display.Submit({{{1}, {2}}, {Duration{140}}, {}}) == PresentationAdmission::Accepted,
			"Worker clear failed");
	clock->time.store(30);
	Eventually([&] { return display.Snapshot().latest->tick == DisplayTick{3}; }, "Worker did not repeat");
	Require(display.Snapshot().latest->kind == PresentationKind::Repeat && display.Snapshot().latest->output == image,
			"Worker did not retain completed output before future clear");
	clock->time.store(110);
	Eventually([&] { return display.Snapshot().latest->tick == DisplayTick{11}; }, "Stalled worker changed phase");
	Require(display.Snapshot().latest->kind == PresentationKind::Clear &&
				display.Snapshot().latest->skippedTicks == 7 &&
				display.Snapshot().latest->time == RuntimeTime{Duration{210}},
			"Worker caught up missed ticks or cleared early");
	Require(retained.output == image && retained.source == FrameKey{{1}, {1}}, "Worker mutated retained reader");
	auto stopped = std::async(std::launch::async, [&] { display.Stop(); });
	Require(stopped.wait_for(1s) == std::future_status::ready, "Worker stop did not cancel its wait");
	stopped.get();
	Require(!display.Snapshot().running && !display.Faulted(), "Clean worker stop failed");
	clock->time.store(1000);
	Require(display.Snapshot().latest->tick == DisplayTick{11}, "Stopped worker progressed");
}

void ExplicitTimeAndRestart()
{
	auto clock = std::make_shared<TestClock>();
	VirtualDisplay display(clock, 1);
	display.Start({7}, {{Duration{100}}, 10ns}, ExecutionMode::Deterministic);
	clock->time.store(1000000);
	Require(!display.Snapshot().latest, "Deterministic display followed host time");
	auto pending = std::make_shared<Output>();
	std::weak_ptr<Output> lifetime = pending;
	Require(display.Submit({{{7}, {1}}, {Duration{100}}, pending}) == PresentationAdmission::Accepted,
			"Queue old generation");
	Require(display.Advance({Duration{120}})->tick == DisplayTick{2}, "Explicit deterministic display time failed");
	display.Stop();
	pending.reset();
	display.Start({8}, {{Duration{200}}, 10ns}, ExecutionMode::Unpaced);
	Require(!lifetime.expired() &&
				display.Submit({{{8}, {1}}, {Duration{200}}, {}}) == PresentationAdmission::QueueFull,
			"Restart freed pending GPU resources or lost retirement budget");
	Require(display.PollRetired() == 1 && !display.Snapshot().latest, "Polling fabricated completion or unpaced time");
	auto owner = lifetime.lock();
	Require(static_cast<bool>(owner), "Pending retired output owner disappeared");
	owner->state.store(CompletionStatus::Complete);
	owner.reset();
	Require(display.PollRetired() == 0 && lifetime.expired(), "Completed retired output was not released");
	Require(display.Submit({{{8}, {1}}, {Duration{200}}, {}}) == PresentationAdmission::Accepted,
			"Retirement did not free capacity");
	display.AdvanceFromFrame({7}, {Duration{10000}});
	Require(!display.Snapshot().latest, "Stale frame advanced a restarted virtual display");
	Require(display.Advance({Duration{200}})->kind == PresentationKind::Clear, "Explicit unpaced clear failed");
	Require(!display.Advance({Duration{199}}), "Explicit display time regressed");
}

void ManualTicksAndResume()
{
	auto clock = std::make_shared<TestClock>();
	VirtualDisplay display(clock);
	Require(!display.Pause() && !display.Paused() && !display.Step() && !display.Resume(HostTime{}, {}),
			"Stopped display accepted manual control");
	display.Start({11}, {{Duration{100}}, 10ns}, ExecutionMode::RealTime, HostTime{});
	Eventually([&] { return display.Snapshot().latest.has_value(); }, "Manual display worker did not start");
	Require(display.Pause() && display.Paused() && display.Pause(), "Pause did not retain active manual state");
	const auto boundary = display.Snapshot();
	Require(boundary.running && boundary.latest->tick == DisplayTick{0} && display.NextTick() == DisplayTick{1},
			"Pause closed producer admission or changed its display boundary");
	auto output = std::make_shared<Output>();
	output->display = &display;
	Require(display.Submit({{{11}, {1}}, {Duration{110}}, output}) == PresentationAdmission::Accepted,
			"Already-admitted GPU work could not submit during pause");
	clock->time.store(1000000);
	display.AdvanceFromFrame({11}, {Duration{10000}});
	Require(display.Snapshot().latest->tick == DisplayTick{0},
			"Inspection time or frame admission advanced manual display");
	const auto pending = display.Step();
	Require(pending && pending->tick == DisplayTick{1} && pending->kind == PresentationKind::Empty,
			"Manual step did not advance exactly one tick or fabricated GPU completion");
	output->state.store(CompletionStatus::Complete);
	const auto selected = display.Step();
	Require(selected && selected->tick == DisplayTick{2} && selected->source == FrameKey{{11}, {1}} &&
				selected->kind == PresentationKind::NewFrame,
			"Manual step did not select completed output");
	Require(display.Submit({{{11}, {2}}, {Duration{130}}, {}}) == PresentationAdmission::Accepted,
			"Manual clear admission failed");
	Require(display.Step()->kind == PresentationKind::Clear, "Manual clear did not display");
	const auto repeat = display.Step();
	Require(repeat->tick == DisplayTick{4} && repeat->kind == PresentationKind::Repeat && !repeat->output &&
				repeat->source == FrameKey{{11}, {2}} && selected->output == output,
			"Manual repeat lost clear identity or changed retained reader");
	Require(!display.Resume(clock->Now(), {4}) && display.Paused(), "Resume accepted a regressing boundary");
	Require(display.Resume(clock->Now(), {5}) && !display.Paused(), "Manual display did not resume");
	Eventually([&] { return display.Snapshot().latest->tick == DisplayTick{5}; },
			   "Resume did not rebase virtual boundary");
	Require(display.Resume(HostTime{}, {}) && !display.Step(),
			"Repeated resume reset pacing or admitted a manual tick");
	clock->time.store(1000009);
	std::this_thread::sleep_for(5ms);
	Require(display.Snapshot().latest->tick == DisplayTick{5}, "Resume caused a catch-up burst");
	clock->time.store(1000010);
	Eventually([&] { return display.Snapshot().latest->tick == DisplayTick{6}; }, "Resumed wall-clock period changed");
	Require(display.Pause() && display.Step()->tick == DisplayTick{7}, "Repeated pause lost display cursor");
	display.Stop();
	Require(!display.Paused() && !display.Step(), "Stopped manual display advanced");

	display.Start({12}, {{Duration{100}}, 10ns}, ExecutionMode::Deterministic);
	Require(display.Pause() && display.NextTick() == DisplayTick{0} && display.Step()->tick == DisplayTick{0},
			"First explicit refresh did not begin at the persistent epoch");
	display.AdvanceFromFrame({12}, {Duration{10000}});
	Require(display.Snapshot().latest->tick == DisplayTick{0}, "Paused deterministic display followed issued frames");
	Require(display.Resume(clock->Now(), {1}), "Deterministic display resume failed");
	display.AdvanceFromFrame({12}, {Duration{120}});
	Require(display.Snapshot().latest->tick == DisplayTick{2}, "Deterministic frame-driven progress did not resume");
	display.Stop();

	clock->time.store(0);
	display.Start({13}, {{Duration{100}}, 10ns}, ExecutionMode::RealTime, HostTime{});
	Eventually([&] { return display.Snapshot().latest.has_value(); }, "Tick-anchor worker did not start");
	Require(display.Pause() && display.Resume(HostTime{}, {100}), "Resume required a negative synthetic host epoch");
	Eventually([&] { return display.Snapshot().latest->tick == DisplayTick{100}; },
			   "Separate host/tick anchors were not honored");
	Require(display.Snapshot().latest->skippedTicks == 99 && !display.Faulted(),
			"Rebased boundary skipped wrong ticks");
	display.Stop();
}

void SharedManualTiming()
{
	auto clock = std::make_shared<TestClock>();
	FrameScheduler scheduler(clock);
	VirtualDisplay display(clock);
	FrameSchedulerConfig config;
	config.epoch = {Duration{100}};
	config.period = 10ns;
	config.hostEpoch = HostTime{};
	scheduler.Start(config);
	display.Start({14}, {config.epoch, config.period}, config.mode, config.hostEpoch);
	FramePrediction first, second, submitted;
	Require(scheduler.WaitFrame(first) == XR_SUCCESS && scheduler.BeginFrame() == XR_SUCCESS,
			"Shared timing initial application frame failed");
	clock->time.store(1200);
	Eventually(
		[&]
		{
			auto state = display.Snapshot();
			return state.latest && state.latest->tick == DisplayTick{120};
		},
		"Shared timing display did not progress independently of application admission");
	auto waiting = std::async(std::launch::async, [&] { return scheduler.WaitFrame(second); });
	Eventually([&] { return scheduler.Snapshot().waitPending; }, "Shared timing paced wait did not enter");
	scheduler.Pause();
	Require(display.Pause(), "Shared timing display pause failed");
	const auto boundary = std::max(scheduler.Snapshot().nextAdmission, display.NextTick());
	Require(boundary == DisplayTick{121} && scheduler.RebasePaused(boundary) == XR_SUCCESS,
			"Shared manual boundary did not account for the advanced display");
	clock->time.store(1000000);
	Require(scheduler.GrantPermits() == XR_SUCCESS && waiting.wait_for(1s) == std::future_status::ready &&
				waiting.get() == XR_SUCCESS && second.displayTick == DisplayTick{122} && second.id == FrameId{2},
			"Strict shared admission used inspection host time or predicted behind the display");
	Require(display.Snapshot().latest->tick == DisplayTick{120} && scheduler.Snapshot().begun->id == first.id,
			"Strict admission advanced display or replaced previously begun work");
	Require(scheduler.EndFrame(submitted) == XR_SUCCESS && submitted.id == first.id &&
				display.Submit({{{14}, first.id}, first.displayTime, {}}) == PresentationAdmission::Accepted,
			"Paused begun work could not finish");
	Require(display.Step()->source == FrameKey{{14}, first.id} && scheduler.BeginFrame() == XR_SUCCESS &&
				scheduler.EndFrame(submitted) == XR_SUCCESS && submitted.id == second.id &&
				display.Submit({{{14}, second.id}, second.displayTime, {}}) == PresentationAdmission::Accepted,
			"Paused pipeline lost an admitted frame identity");
	Require(display.Step()->source == FrameKey{{14}, second.id}, "Independent display step lost the strict submission");
	const auto resumedBoundary = std::max(scheduler.Snapshot().nextAdmission, display.NextTick());
	const auto hostAnchor = clock->Now();
	Require(resumedBoundary == DisplayTick{123} && display.Resume(hostAnchor, resumedBoundary) &&
				scheduler.Resume(hostAnchor, resumedBoundary) == XR_SUCCESS,
			"Shared resume anchors failed");
	Eventually([&] { return display.Snapshot().latest->tick == resumedBoundary; },
			   "Shared display resume boundary changed");
	Require(scheduler.WaitFrame(first) == XR_SUCCESS && first.displayTick == DisplayTick{124} &&
				scheduler.BeginFrame() == XR_SUCCESS,
			"Shared resumed application prediction changed phase");
	waiting = std::async(std::launch::async, [&] { return scheduler.WaitFrame(second); });
	Eventually([&] { return scheduler.Snapshot().waitPending; }, "Shared resumed pacing did not enter");
	clock->time.store(1000009);
	Require(waiting.wait_for(5ms) == std::future_status::timeout && display.Snapshot().latest->tick == DisplayTick{123},
			"Shared resume caused a prediction or display catch-up burst");
	clock->time.store(1000010);
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_SUCCESS &&
				second.displayTick == DisplayTick{125},
			"Shared resumed pacing did not preserve one refresh period");
	Eventually([&] { return display.Snapshot().latest->tick == DisplayTick{124}; }, "Resumed display phase diverged");
	scheduler.Stop();
	display.Stop();
}

void Fault()
{
	auto clock = std::make_shared<TestClock>();
	VirtualDisplay display(clock);
	display.Start({1}, {{Duration{100}}, 10ns}, ExecutionMode::RealTime);
	Eventually([&] { return display.Snapshot().latest.has_value(); }, "Fault test worker not started");
	clock->fail.store(true);
	Eventually([&] { return display.Faulted() && !display.Snapshot().running; }, "Worker failure not reported");
	display.Stop();
	clock->fail.store(false);
	display.Start({2}, {{Duration{std::numeric_limits<std::int64_t>::max() - 5}}, 10ns}, ExecutionMode::RealTime,
				  HostTime{});
	Eventually([&] { return display.Faulted() && !display.Snapshot().running; }, "Time overflow fabricated progress");
	display.Stop();
}

void SessionClear()
{
	auto clock = std::make_shared<TestClock>();
	Instance instance(reinterpret_cast<XrInstance>(500), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(501), instance, clock);
	FrameSchedulerConfig config;
	config.epoch = {Duration{100}};
	config.period = 10ns;
	config.hostEpoch = HostTime{};
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(config.epoch) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Real-time session display lifecycle");
	FramePrediction frame;
	Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
				session.EndFrame(frame) == XR_SUCCESS,
			"Real-time empty submission failed");
	const auto record = session.History().Find({session.Snapshot().generation, frame.id});
	Require(record && record->stage == FrameStage::Submitted, "Submitted identity did not retain CPU history");
	clock->time.store(10);
	Eventually(
		[&]
		{
			auto state = session.Display().Snapshot();
			return state.latest && state.latest->source == record->key;
		},
		"Empty frame was not independently displayed without another XR call");
	Require(session.Display().Snapshot().latest->kind == PresentationKind::Clear &&
				session.Display().Snapshot().latest->time == frame.displayTime,
			"Empty frame displayed at wrong tick");
	session.Close();
	Require(!session.Display().Snapshot().running && session.Snapshot().closed, "Session did not join display worker");
}

void DeterministicSessionCapacity()
{
	auto clock = std::make_shared<TestClock>();
	Instance instance(reinterpret_cast<XrInstance>(510), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(511), instance, clock);
	FrameSchedulerConfig config;
	config.epoch = {Duration{100}};
	config.period = 10ns;
	config.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(config.epoch) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Deterministic session display lifecycle");
	FramePrediction frame;
	for (std::uint64_t index = 1; index <= 64; ++index)
		Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
					session.EndFrame(frame) == XR_SUCCESS,
				"Ordinary deterministic clears exhausted bounded queue");
	const auto state = session.Display().Snapshot();
	Require(state.latest && state.latest->source == FrameKey{session.Snapshot().generation, {63}} &&
				state.queued.size() == 1 && state.queued.front().frame == FrameId{64},
			"Deterministic frame admission did not explicitly advance prior virtual ticks");
	Require(session.Display().Advance(frame.displayTime)->source == FrameKey{session.Snapshot().generation, {64}},
			"Explicit final deterministic display step failed");
	XrFrameEndInfo future{XR_TYPE_FRAME_END_INFO, nullptr, 10000, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, nullptr};
	for (std::size_t index = 0; index < state.capacity; ++index)
		Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
					session.EndFrame(frame, &future) == XR_SUCCESS,
				"Future clear queue rejected available capacity");
	Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
			"Capacity test frame was not begun");
	const auto key = FrameKey{session.Snapshot().generation, frame.id};
	Require(session.EndFrame(frame, &future) == XR_ERROR_LIMIT_REACHED &&
				session.Scheduler().Snapshot().begun->id == key.frame &&
				session.History().Find(key)->stage == FrameStage::Begun,
			"Full display queue consumed begun frame or committed history");
	Require(session.Display().Advance({Duration{10000}})->kind == PresentationKind::Clear &&
				session.EndFrame(frame, &future) == XR_SUCCESS,
			"Capacity retry lost frame identity");
}

class TeardownOutput final : public FrameOutput
{
  public:
	TeardownOutput(Runtime& owner, Session& value, XrSession identity)
		: runtime(owner), session(value), handle(identity)
	{
	}
	CompletionStatus PollCompletion() const noexcept override
	{
		entered.store(true);
		while (!release.load())
			std::this_thread::sleep_for(1ms);
		observedClosed.store(!runtime.FindSession(handle) && session.Snapshot().closed);
		return CompletionStatus::Pending;
	}
	Runtime& runtime;
	Session& session;
	XrSession handle;
	mutable std::atomic<bool> entered{false};
	mutable std::atomic<bool> observedClosed{false};
	std::atomic<bool> release{false};
};

void GraphTeardown()
{
	for (int operation = 0; operation < 3; ++operation)
	{
		Runtime runtime(ExtensionRegistry({}));
		Runtime::InstanceRef instance;
		Runtime::SessionRef session;
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
		std::strcpy(create.applicationInfo.applicationName, "Worker teardown");
		auto clock = std::make_shared<TestClock>();
		Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
					runtime.CreateSession(instance, session, clock) == XR_SUCCESS,
				"Worker graph creation failed");
		FrameSchedulerConfig config;
		config.epoch = {Duration{100}};
		config.period = 10ns;
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(config.epoch) == XR_SUCCESS && session->Begin(begin, config) == XR_SUCCESS,
				"Worker graph lifecycle failed");
		auto output = std::make_shared<TeardownOutput>(runtime, *session.Shared(), session);
		Require(session->Display().Submit({{session->Snapshot().generation, {1}}, {Duration{110}}, output}) ==
					PresentationAdmission::Accepted,
				"Worker graph output failed");
		clock->time.store(10);
		Eventually([&] { return output->entered.load(); }, "Worker did not observe output");
		auto closing =
			std::async(std::launch::async,
					   [&]
					   {
						   if (operation == 0)
							   Require(runtime.DestroySession(session) == XR_SUCCESS, "Worker session destruction");
						   if (operation == 1)
							   Require(runtime.DestroyInstance(instance) == XR_SUCCESS, "Worker instance destruction");
						   if (operation == 2)
							   runtime.Close();
					   });
		Eventually([&] { return !runtime.FindSession(session) && session->Snapshot().closed; },
				   "Graph did not invalidate handles before join");
		Eventually([&] { return !session->Display().Snapshot().running; },
				   "Display admission remained open during join");
		Require(session->Display().Submit({{session->Snapshot().generation, {2}}, {Duration{120}}, {}}) ==
						PresentationAdmission::Closed &&
					closing.wait_for(5ms) == std::future_status::timeout,
				"Teardown admitted new work or failed to join observer");
		output->release.store(true);
		Require(closing.wait_for(1s) == std::future_status::ready, "Graph joined worker while holding queried locks");
		closing.get();
		Require(output->observedClosed.load() && !session->Display().Snapshot().running,
				"Worker teardown did not retain session state through observation");
	}
}
} // namespace

int main()
{
	Worker();
	ExplicitTimeAndRestart();
	ManualTicksAndResume();
	SharedManualTiming();
	Fault();
	SessionClear();
	DeterministicSessionCapacity();
	GraphTeardown();
	std::cout << "Virtual display worker tests passed; no hardware scan-out exercised\n";
}
