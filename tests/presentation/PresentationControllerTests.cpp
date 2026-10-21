#include "../TestSupport.h"
#include "presentation/FrameHistory.h"
#include "presentation/PresentationController.h"

#include <atomic>
#include <future>
#include <iostream>
#include <limits>
#include <stdexcept>
#include <thread>

namespace
{
using namespace simxr;

class Output final : public FrameOutput
{
  public:
	explicit Output(CompletionStatus status = CompletionStatus::Pending) : state(status) {}
	~Output() override
	{
		if (destroy)
			destroy();
	}
	std::function<void()> destroy;
	CompletionStatus PollCompletion() const noexcept override
	{
		++polls;
		if (inspect)
			inspect->Snapshot();
		return state.load();
	}
	std::atomic<CompletionStatus> state;
	mutable std::atomic<int> polls{0};
	PresentationController* inspect = nullptr;
};

PresentationSubmission Submission(std::uint64_t frame, std::int64_t target,
								  std::shared_ptr<const FrameOutput> output = {}, std::uint64_t generation = 1)
{
	return {{{generation}, {frame}}, {Duration{target}}, std::move(output)};
}

void Selection()
{
	PresentationController controller(6);
	controller.Start({1}, {{Duration{100}}, Duration{10}});
	auto pending = std::make_shared<Output>();
	auto future = std::make_shared<Output>(CompletionStatus::Complete);
	pending->inspect = &controller; // Backend observations execute outside the controller mutex.
	Require(controller.Submit(Submission(1, 100, pending)) == PresentationAdmission::Accepted &&
				controller.Submit(Submission(2, 130, future)) == PresentationAdmission::Accepted,
			"Queue frames");
	Require(!controller.Advance({Duration{99}}), "Display ran before epoch");
	auto tick = controller.Advance({Duration{100}});
	Require(tick && tick->kind == PresentationKind::Empty && tick->tick == DisplayTick{0} && !tick->source,
			"Pending frame or future target presented");
	Require(!controller.Advance({Duration{109}}), "Same display tick emitted twice");
	pending->state.store(CompletionStatus::Complete);
	tick = controller.Advance({Duration{110}});
	Require(tick && tick->kind == PresentationKind::NewFrame && tick->source == FrameKey{{1}, {1}} &&
				tick->output == pending,
			"Completed eligible frame not selected");
	const auto retained = *tick;
	tick = controller.Advance({Duration{120}});
	Require(tick && tick->kind == PresentationKind::Repeat && tick->source == retained.source &&
				tick->output == pending,
			"Missed deadline did not repeat retained output");
	tick = controller.Advance({Duration{130}});
	Require(tick && tick->kind == PresentationKind::NewFrame && tick->source == FrameKey{{1}, {2}} &&
				tick->output == future,
			"Future target selected at wrong tick");
	auto late = std::make_shared<Output>();
	auto newest = std::make_shared<Output>(CompletionStatus::Complete);
	Require(controller.Submit(Submission(3, 140, late)) == PresentationAdmission::Accepted &&
				controller.Submit(Submission(4, 140, newest)) == PresentationAdmission::Accepted,
			"Queue out-of-order completions");
	tick = controller.Advance({Duration{140}});
	Require(tick && tick->source == FrameKey{{1}, {4}} && controller.Snapshot().retiring.size() == 1,
			"Latest completed submission was not selected");
	late->state.store(CompletionStatus::Complete);
	tick = controller.Advance({Duration{150}});
	Require(tick && tick->kind == PresentationKind::Repeat && tick->source == FrameKey{{1}, {4}} &&
				controller.Snapshot().retiring.empty(),
			"Late completion regressed selected source");
	Require(controller.Submit(Submission(5, 180)) == PresentationAdmission::Accepted, "Queue clear");
	tick = controller.Advance({Duration{170}});
	Require(tick && tick->kind == PresentationKind::Repeat && tick->skippedTicks == 1 &&
				tick->source == FrameKey{{1}, {4}},
			"Clear applied before target or missed ticks caught up");
	tick = controller.Advance({Duration{180}});
	Require(tick && tick->kind == PresentationKind::Clear && tick->source == FrameKey{{1}, {5}} && !tick->output,
			"Eligible zero-layer frame did not clear");
	tick = controller.Advance({Duration{190}});
	Require(tick && tick->kind == PresentationKind::Repeat && tick->source == FrameKey{{1}, {5}} && !tick->output,
			"Old image reappeared after clear");
	auto lateAfterClear = std::make_shared<Output>();
	Require(controller.Submit(Submission(6, 200, lateAfterClear)) == PresentationAdmission::Accepted &&
				controller.Submit(Submission(7, 200)) == PresentationAdmission::Accepted,
			"Queue pending image and newer clear");
	tick = controller.Advance({Duration{200}});
	Require(tick && tick->kind == PresentationKind::Clear && tick->source == FrameKey{{1}, {7}},
			"Newer clear lost to pending image");
	lateAfterClear->state.store(CompletionStatus::Complete);
	tick = controller.Advance({Duration{210}});
	Require(tick && tick->kind == PresentationKind::Repeat && tick->source == FrameKey{{1}, {7}} && !tick->output,
			"Older image reappeared after clear on late completion");
	auto oldFuture = std::make_shared<Output>(CompletionStatus::Complete);
	Require(controller.Submit(Submission(8, 300, oldFuture)) == PresentationAdmission::Accepted &&
				controller.Submit(Submission(9, 220)) == PresentationAdmission::Accepted,
			"Queue future image before earlier clear");
	tick = controller.Advance({Duration{220}});
	Require(tick && tick->kind == PresentationKind::Clear && tick->source == FrameKey{{1}, {9}},
			"Submission order did not define latest eligible clear");
	tick = controller.Advance({Duration{300}});
	Require(tick && tick->kind == PresentationKind::Repeat && tick->source == FrameKey{{1}, {9}} && !tick->output,
			"Superseded future target revived old image after clear");
	Require(retained.output == pending && retained.source == FrameKey{{1}, {1}},
			"Later ticks mutated retained CPU reader");
}

void FailureAndShutdown()
{
	PresentationController controller(2);
	const DisplayTimeline timeline{{Duration{100}}, Duration{10}};
	controller.Start({10}, timeline);
	auto pending = std::make_shared<Output>();
	auto failed = std::make_shared<Output>(CompletionStatus::Failed);
	std::weak_ptr<Output> pendingLifetime = pending;
	Require(controller.Submit(Submission(1, 100, pending, 9)) == PresentationAdmission::StaleGeneration,
			"Stale session generation accepted");
	Require(controller.Submit(Submission(0, 100, pending, 10)) == PresentationAdmission::Invalid &&
				controller.Submit(Submission(1, 0, pending, 10)) == PresentationAdmission::Invalid,
			"Malformed submission accepted");
	Require(controller.Submit(Submission(1, 100, pending, 10)) == PresentationAdmission::Accepted &&
				controller.Submit(Submission(1, 100, pending, 10)) == PresentationAdmission::Invalid &&
				controller.Submit(Submission(2, 100, failed, 10)) == PresentationAdmission::Accepted,
			"Submission order or duplicate validation");
	Require(controller.Submit(Submission(3, 100, {}, 10)) == PresentationAdmission::QueueFull,
			"Queue capacity ignored");
	auto tick = controller.Advance({Duration{100}});
	Require(tick && tick->kind == PresentationKind::Empty && controller.Snapshot().queued.size() == 1 &&
				failed->state.load() == CompletionStatus::Failed && pending->state.load() == CompletionStatus::Pending,
			"Failure presented an image or fabricated completion");
	controller.Stop();
	pending.reset();
	Require(!pendingLifetime.expired() && !controller.Advance({Duration{1000}}) &&
				controller.Submit(Submission(3, 100, {}, 10)) == PresentationAdmission::Closed &&
				controller.PollRetired() == 1,
			"Shutdown discarded pending owner or advanced presentation");
	controller.Start({11}, timeline);
	Require(controller.Submit(Submission(1, 100, {}, 11)) == PresentationAdmission::Accepted &&
				controller.Submit(Submission(2, 100, {}, 11)) == PresentationAdmission::QueueFull,
			"Restart ignored pending retirement budget");
	tick = controller.Advance({Duration{100}});
	Require(tick && tick->kind == PresentationKind::Clear && tick->source == FrameKey{{11}, {1}},
			"Restart reused old session source");
	auto retained = pendingLifetime.lock();
	Require(static_cast<bool>(retained), "Pending retirement owner disappeared");
	retained->state.store(CompletionStatus::Failed);
	retained.reset();
	Require(controller.PollRetired() == 0 && pendingLifetime.expired(), "Terminal retirement retained CPU owner");
	bool rejected = false;
	try
	{
		controller.Start({11}, timeline);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && controller.Snapshot().generation == SessionGeneration{11}, "Generation reused");
}

void PhaseAndConcurrency()
{
	PresentationController controller;
	controller.Start({1}, {{Duration{100}}, Duration{10}});
	auto tick = controller.Advance({Duration{159}});
	Require(tick && tick->tick == DisplayTick{5} && tick->time == RuntimeTime{Duration{150}} && tick->skippedTicks == 5,
			"Stall changed display phase");
	Require(!controller.Advance({Duration{159}}) && !controller.Advance({Duration{120}}),
			"Repeated or backward tick emitted");
	tick = controller.Advance({Duration{160}});
	Require(tick && tick->tick == DisplayTick{6} && tick->skippedTicks == 0, "Display caught up after stall");
	std::atomic<bool> stop = false;
	auto reader = std::async(std::launch::async,
							 [&]
							 {
								 while (!stop.load())
								 {
									 const auto snapshot = controller.Snapshot();
									 Require(snapshot.queued.size() + snapshot.retiring.size() <= snapshot.capacity,
											 "Partial or unbounded queue snapshot");
								 }
							 });
	for (std::uint64_t frame = 1; frame <= 100; ++frame)
	{
		auto output = std::make_shared<Output>();
		Require(controller.Submit(Submission(frame, 100, output)) == PresentationAdmission::Accepted,
				"Concurrent publication");
		output->state.store(CompletionStatus::Complete);
		tick = controller.Advance({Duration{160 + static_cast<std::int64_t>(frame) * 10}});
		Require(tick && tick->source == FrameKey{{1}, {frame}}, "Concurrent selection lost complete frame");
	}
	stop.store(true);
	reader.get();
	controller.Stop();
	controller.PollRetired();
	PresentationController maximum;
	maximum.Start({2}, {{Duration{1}}, Duration{7}});
	tick = maximum.Advance({Duration{std::numeric_limits<std::int64_t>::max()}});
	Require(tick && tick->time.value.count() <= std::numeric_limits<std::int64_t>::max(),
			"Latest-due tick overflowed at time limit");
}

void Reservation()
{
	PresentationController controller(1);
	controller.Start({1}, {{Duration{100}}, Duration{10}});
	bool failed = false;
	try
	{
		controller.Submit(Submission(1, 100), [] { throw std::bad_alloc{}; });
	}
	catch (const std::bad_alloc&)
	{
		failed = true;
	}
	Require(failed && controller.Snapshot().queued.empty() && controller.Snapshot().lastSubmitted == FrameId{},
			"Failed frame commit consumed presentation reservation");
	bool committed = false;
	Require(controller.Submit(Submission(1, 100), [&] { committed = true; }) == PresentationAdmission::Accepted &&
				committed,
			"Reservation retry lost identity or did not commit frame state");
	bool called = false;
	Require(controller.Submit(Submission(2, 100), [&] { called = true; }) == PresentationAdmission::QueueFull &&
				!called,
			"Capacity rejection consumed frame state");
	Require(controller.Advance({Duration{100}})->source == FrameKey{{1}, {1}}, "Committed frame was not published");
}
void LiveClearMailbox()
{
	PresentationController controller(1);
	controller.Start({1}, {{Duration{100}}, Duration{10}});
	auto pending = std::make_shared<Output>();
	pending->inspect = &controller;
	Require(controller.Submit(Submission(1, 100, pending)) == PresentationAdmission::Accepted, "Fill live-clear queue");
	unsigned metadataReleases = 0;
	auto clearSubmission = Submission(2, 120);
	clearSubmission.metadata = std::shared_ptr<const FrameRecord>(new FrameRecord{},
																  [&](const FrameRecord* record)
																  {
																	  controller.Snapshot();
																	  ++metadataReleases;
																	  delete record;
																  });
	const auto firstClear = controller.SubmitLiveClearWithMetadata(std::move(clearSubmission), {});
	Require(firstClear == PresentationAdmission::Accepted && controller.Snapshot().queued.size() == 1 &&
				controller.Snapshot().liveClear == FrameKey{{1}, {2}} && controller.Retain().size() == 2,
			"Live clear did not use its bounded CPU-only mailbox");
	Require(controller.Advance({Duration{100}})->kind == PresentationKind::Empty, "Future clear displayed early");
	Require(controller.SubmitLiveClearWithMetadata(Submission(3, 130), {}) == PresentationAdmission::Accepted &&
				controller.Snapshot().coalescedLiveClears == 1 && metadataReleases == 1,
			"Latest live clear was not explicitly coalesced");
	Require(controller.Advance({Duration{120}})->kind == PresentationKind::Empty,
			"Replaced clear fabricated an obsolete target tick");
	bool failed = false;
	try
	{
		controller.SubmitLiveClearWithMetadata(Submission(4, 140),
											   []() -> std::shared_ptr<const FrameRecord> { throw std::bad_alloc{}; });
	}
	catch (const std::bad_alloc&)
	{
		failed = true;
	}
	Require(failed && controller.Snapshot().liveClear == FrameKey{{1}, {3}} &&
				controller.Snapshot().lastSubmitted == FrameId{3} && controller.Snapshot().coalescedLiveClears == 1,
			"Failed mailbox callback changed retained clear or consumed identity");
	Require(controller.SubmitLiveClearWithMetadata(Submission(4, 140), {}) == PresentationAdmission::Accepted &&
				controller.Snapshot().coalescedLiveClears == 2,
			"Mailbox callback rollback prevented same-identity retry");
	const auto clear = controller.Advance({Duration{140}});
	Require(clear && clear->kind == PresentationKind::Clear && clear->source == FrameKey{{1}, {4}} && !clear->output &&
				!controller.Snapshot().liveClear && controller.Snapshot().retiring.size() == 1,
			"Live clear waited for pending GPU work or fabricated its source identity");
	pending->state.store(CompletionStatus::Complete);
	const auto repeated = controller.Advance({Duration{150}});
	Require(repeated && repeated->kind == PresentationKind::Repeat && repeated->source == clear->source &&
				!repeated->output && controller.Snapshot().queued.empty() && controller.Snapshot().retiring.empty(),
			"Old GPU completion resurrected an image after live clear");
	controller.Stop();
	controller.Start({2}, {{Duration{200}}, Duration{10}});
	Require(!controller.Snapshot().liveClear && controller.Snapshot().coalescedLiveClears == 0 &&
				controller.SubmitLiveClearWithMetadata(Submission(1, 200, {}, 1), {}) ==
					PresentationAdmission::StaleGeneration,
			"Mailbox state survived generation restart");
	bool destroyed = false;
	auto invalid = std::make_shared<Output>();
	invalid->destroy = [&]
	{
		controller.Snapshot();
		destroyed = true;
	};
	const auto rejected = controller.SubmitLiveClearWithMetadata(Submission(1, 200, std::move(invalid), 2), {});
	Require(rejected == PresentationAdmission::Invalid && destroyed,
			"Invalid live-clear GPU owner was released under controller lock");
}

} // namespace

int main()
{
	Selection();
	FailureAndShutdown();
	PhaseAndConcurrency();
	Reservation();
	LiveClearMailbox();
	std::cout << "Portable presentation selection tests passed; no hardware display exercised\n";
}
