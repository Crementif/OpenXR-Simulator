#include "../TestSupport.h"
#include "presentation/DebugController.h"
#include "runtime/Instance.h"
#include "runtime/Session.h"

#include <atomic>
#include <cstdlib>
#include <functional>
#include <future>
#include <iostream>
#include <new>
#include <thread>

namespace
{
thread_local int debugAllocationCountdown = -1;
}
[[gnu::noinline]] void* operator new(std::size_t size)
{
	if (debugAllocationCountdown >= 0 && debugAllocationCountdown-- == 0)
		throw std::bad_alloc{};
	if (void* memory = std::malloc(size ? size : 1))
		return memory;
	throw std::bad_alloc{};
}
[[gnu::noinline]] void operator delete(void* memory) noexcept
{
	std::free(memory);
}
[[gnu::noinline]] void operator delete(void* memory, std::size_t) noexcept
{
	std::free(memory);
}

using namespace simxr;
using namespace std::chrono_literals;

namespace
{
class TestClock final : public Clock
{
  public:
	HostTime Now() const override
	{
		return HostTime{Duration{time.load()}};
	}
	void WaitUntil(std::condition_variable& changed, std::unique_lock<std::mutex>& lock, HostTime) override
	{
		changed.wait_for(lock, 1ms);
	}
	std::atomic<std::int64_t> time{0};
};
class Output final : public FrameOutput
{
  public:
	~Output() override
	{
		if (onDestroy)
			onDestroy();
	}
	std::function<void()> onDestroy;
	CompletionStatus PollCompletion() const noexcept override
	{
		if (controller)
			controller->Snapshot();
		return state.load();
	}
	std::atomic<CompletionStatus> state{CompletionStatus::Pending};
	std::shared_ptr<FrameReservation> reservation;
	DebugController* controller = nullptr;
};
struct Fixture
{
	std::shared_ptr<TestClock> clock = std::make_shared<TestClock>();
	std::shared_ptr<Instance> instance = std::make_shared<Instance>(
		reinterpret_cast<XrInstance>(100), std::span<const char* const>{}, XR_MAKE_VERSION(1, 0, 27));
	std::shared_ptr<Session> session = std::make_shared<Session>(reinterpret_cast<XrSession>(101), instance, clock);
	FramePool pool;
	explicit Fixture(std::size_t capacity = 8, bool configure = true, bool layerViews = false) : pool(capacity)
	{
		FrameSchedulerConfig timing;
		timing.epoch = {100ns};
		timing.period = 10ns;
		timing.mode = ExecutionMode::Deterministic;
		if (configure)
			Require(session->ConfigureCapturePool(pool, layerViews) == XR_SUCCESS, "Configure capture pool");
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS,
				"Begin test session");
	}
	FramePrediction Issue(bool begin = true)
	{
		FramePrediction prediction;
		Require(session->WaitFrame(prediction) == XR_SUCCESS, "Issue test frame");
		if (begin)
			Require(session->BeginFrame() == XR_SUCCESS, "Begin test frame");
		return prediction;
	}
	std::shared_ptr<Output> Image(const FramePrediction& prediction, CaptureTarget target = {})
	{
		auto reservation =
			session->Captures().ProducerReservation({session->Snapshot().generation, prediction.id}, target);
		if (!reservation)
			reservation = pool.Reserve();
		Require(reservation && pool.Consume(reservation), "Fake output did not consume reserved capacity");
		auto output = std::make_shared<Output>();
		output->reservation = std::move(reservation);
		return output;
	}
	void Submit(FramePrediction prediction, std::shared_ptr<Output> output = {})
	{
		if (!output)
		{
			Require(session->EndFrame(prediction) == XR_SUCCESS, "Submit clear frame");
			return;
		}
		auto metadata =
			std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
												std::vector<LayerDescription>{ProjectionLayerDescription{}});
		XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,			 nullptr, prediction.displayTime.value.count(),
						   XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		  nullptr};
		Require(session->EndFrame(prediction, &end, metadata, std::move(output), [] { return XR_SUCCESS; }) ==
					XR_SUCCESS,
				"Submit fake producer with immutable metadata");
	}
};
DebugControllerConfig ManualConfig()
{
	DebugControllerConfig config;
	config.startWorker = false;
	config.leaseDuration = 1s;
	return config;
}
DebugOperationId Command(DebugController& controller, DebugLeaseId lease, std::uint64_t id, DebugCommand command,
						 std::uint32_t lookahead = 1, DebugOperationId target = {}, CaptureId capture = {},
						 CaptureTarget captureTarget = {})
{
	const auto result = controller.Submit(lease, {{id}, command, lookahead, captureTarget, target, capture});
	Require(result.result == DebugAdmission::Accepted && result.operation.value, "Command admission failed");
	return result.operation;
}
DebugOperation Status(DebugController& controller, DebugOperationId operation)
{
	const auto status = controller.Read(operation);
	Require(status.has_value(), "Missing operation status");
	return *status;
}
void DisplayStepAllocationRecovery()
{
	unsigned failures = 0;
	for (int allocation = 0; allocation < 256; ++allocation)
	{
		Fixture fixture(4);
		const auto frame = fixture.Issue();
		auto image = fixture.Image(frame);
		image->state = CompletionStatus::Complete;
		fixture.Submit(frame, image);
		DebugController controller(fixture.session, fixture.clock, ManualConfig());
		const auto lease = controller.Acquire()->id;
		Command(controller, lease, 1, DebugCommand::Pause);
		controller.Poll();
		const auto step = Command(controller, lease, 2, DebugCommand::DisplayStep);
		debugAllocationCountdown = allocation;
		controller.Poll();
		const bool injected = debugAllocationCountdown < 0;
		debugAllocationCountdown = -1;
		for (const auto capture : fixture.session->Captures().Snapshot().captures)
			Require(fixture.session->Captures().Read(capture)->phase != SnapshotPhase::Reserved,
					"Failed display step stranded a reserved capture");
		controller.Poll();
		const auto status = Status(controller, step);
		Require(status.phase == DebugPhase::Complete || status.phase == DebugPhase::Failed,
				"Display step did not settle after allocation recovery");
		if (status.phase == DebugPhase::Failed)
		{
			const auto retry = Command(controller, lease, 3, DebugCommand::DisplayStep);
			controller.Poll();
			Require(Status(controller, retry).phase == DebugPhase::Complete,
					"Failed display step exhausted capacity needed by the next request");
		}
		if (!injected)
		{
			Require(failures > 0, "Display step allocation sweep did not exercise failures");
			std::cout << "Display step allocation sweep: " << failures << " failures\n";
			return;
		}
		++failures;
	}
	throw std::runtime_error("Display step allocation sweep did not complete");
}
void RealtimeRecordingBudget()
{
	Fixture fixture;
	DebugController controller(fixture.session, fixture.clock, ManualConfig());
	const auto lease = controller.Acquire();
	Require(lease.has_value(), "Acquire recording lease");
	const auto start = Command(controller, lease->id, 1, DebugCommand::StartImageRecording);
	controller.Poll();
	Require(Status(controller, start).progress == DebugProgress::RecordingStarted &&
		!controller.Snapshot().pipeline.paused, "Recording changed application pacing");
	const auto first = fixture.Issue();
	fixture.Submit(first);
	const auto oldCapture = controller.Snapshot().imageRecording.captures.front();
	const auto released = Command(controller, lease->id, 2, DebugCommand::Release, 1, {}, oldCapture);
	controller.Poll();
	Require(Status(controller, released).progress == DebugProgress::Released &&
		controller.Snapshot().imageRecording.captures.size() == 2,
		"Acknowledged recording capture did not replenish its bounded reservation");
	fixture.Submit(fixture.Issue());
	fixture.Submit(fixture.Issue());
	const auto missed = fixture.Issue();
	fixture.Submit(missed);
	const auto state = controller.Snapshot();
	Require(state.imageRecording.phase == ImageRecordingPhase::Overrun &&
		state.imageRecording.missed == FrameKey{fixture.session->Snapshot().generation, missed.id} &&
		state.imageRecording.captured == 3 && !state.pipeline.paused && !state.pipeline.permits,
		"Slow recorder silently dropped a frame or changed normal pacing");
	fixture.clock->time = 2000000000;
	controller.Poll();
	Require(controller.Snapshot().imageRecording.captures.empty() && !controller.Snapshot().manual,
		"Expired recording lease kept reservations or paused the application");
	FrameSchedulerConfig timing;
	timing.epoch = {500ns};
	timing.period = 10ns;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(fixture.session->RequestExit(timing.epoch) == XR_SUCCESS && fixture.session->End(timing.epoch) == XR_SUCCESS &&
		fixture.session->Ready(timing.epoch) == XR_SUCCESS && fixture.session->Begin(begin, timing) == XR_SUCCESS,
		"Restart recording session");
	const auto restarted = controller.Snapshot().imageRecording;
	Require(restarted.phase == ImageRecordingPhase::Idle && !restarted.captured && !restarted.capacity &&
		!restarted.requested && !restarted.missed && restarted.captures.empty(),
		"Recording state from an old generation escaped into the replacement run");
}

void Pause(DebugController& controller, DebugLeaseId lease)
{
	const auto paused = Command(controller, lease, 1, DebugCommand::Pause);
	controller.Poll();
	Require(Status(controller, paused).phase == DebugPhase::Complete && controller.Snapshot().manual,
			"Empty pause did not settle");
}

void IdempotenceAndReclamation()
{
	Fixture fixture;
	auto config = ManualConfig();
	config.ledgerCapacity = 4;
	auto controllerOwner = std::make_unique<DebugController>(fixture.session, fixture.clock, config);
	auto& controller = *controllerOwner;
	Require(controller.Snapshot().composedEyes && !controller.Snapshot().layerViews, "Capture capability mismatch");
	const auto lease = controller.Acquire();
	Require(lease && !controller.Acquire(), "Stepping lease was not exclusive");
	Pause(controller, lease->id);
	DebugRequest request{{2}, DebugCommand::StrictAdmission};
	std::vector<std::future<DebugSubmission>> retries;
	for (unsigned index = 0; index < 8; ++index)
		retries.push_back(std::async(std::launch::async, [&] { return controller.Submit(lease->id, request); }));
	DebugOperationId strict;
	unsigned accepted = 0;
	for (auto& retry : retries)
	{
		const auto result = retry.get();
		accepted += result.result == DebugAdmission::Accepted;
		Require(result.result == DebugAdmission::Accepted || result.result == DebugAdmission::Duplicate,
				"Concurrent identical retry rejected");
		if (!strict.value)
			strict = result.operation;
		Require(strict == result.operation, "Duplicate allocated a second operation ID");
	}
	Require(accepted == 1 && !fixture.session->Scheduler().Snapshot().permits, "Command executed on control thread");
	auto conflict = request;
	conflict.command = DebugCommand::Step;
	Require(controller.Submit(lease->id, conflict).result == DebugAdmission::Conflict, "Conflicting retry accepted");
	controller.Poll();
	Require(fixture.session->Scheduler().Snapshot().permits == 1 && fixture.pool.InUse() == 1 &&
				!controller.Acknowledge(lease->id, {2}),
			"Pending command acknowledged or duplicate granted twice");
	const auto frame = fixture.Issue();
	controller.Poll();
	Require(Status(controller, strict).progress == DebugProgress::Issued && !controller.Snapshot().inspected,
			"Strict admission moved inspection cursor or did not observe issue");
	fixture.Submit(frame);
	const auto inspected = Command(controller, lease->id, 3, DebugCommand::Step);
	controller.Poll();
	const auto record = Status(controller, inspected);
	Require(record.progress == DebugProgress::Clear && record.frame->frame == frame.id && record.capture &&
				!fixture.session->Scheduler().Snapshot().permits,
			"Retained clear inspection issued more work");
	const auto displayed = Command(controller, lease->id, 4, DebugCommand::DisplayStep);
	controller.Poll();
	Require(Status(controller, displayed).display->tick == DisplayTick{0} &&
				Status(controller, displayed).display->kind == PresentationKind::Empty,
			"Display diagnostic advanced more than one refresh");
	Require(controller.Submit(lease->id, {{5}, DebugCommand::Resume}).result == DebugAdmission::Capacity,
			"Unacknowledged ledger was silently evicted");
	Require(controller.Acknowledge(lease->id, {3}) && !controller.Read(inspected),
			"Terminal ledger reclamation failed");
	Require(controller.Submit(lease->id, request).result == DebugAdmission::Retired &&
				!fixture.session->Scheduler().Snapshot().permits,
			"Acknowledged retry executed again");
	const auto released = Command(controller, lease->id, 5, DebugCommand::Release, 1, {}, *record.capture);
	controller.Poll();
	Require(Status(controller, released).progress == DebugProgress::Released &&
				!fixture.session->Captures().Read(*record.capture),
			"Explicit release did not remove capture lookup");
}

void LongRunAcknowledgement()
{
	Fixture fixture;
	auto config = ManualConfig();
	config.ledgerCapacity = 4;
	auto owner = std::make_unique<DebugController>(fixture.session, fixture.clock, config);
	auto& controller = *owner;
	const auto lease = controller.Acquire()->id;
	Pause(controller, lease);
	Require(controller.Acknowledge(lease, {1}), "Acknowledge initial pause");
	for (std::uint64_t tick = 0; tick < 64; ++tick)
	{
		const auto step = Command(controller, lease, tick * 2 + 2, DebugCommand::DisplayStep);
		controller.Poll();
		const auto status = Status(controller, step);
		Require(status.display && status.display->tick == DisplayTick{tick} && status.capture,
				"Long run display advancement lost identity");
		Command(controller, lease, tick * 2 + 3, DebugCommand::Release, 1, {}, *status.capture);
		controller.Poll();
		Require(controller.Acknowledge(lease, {tick * 2 + 3}) && controller.Snapshot().commands == 0 &&
					controller.Snapshot().retention.captures.empty(),
				"Long-run acknowledgement failed to reclaim bounded records");
		Require(controller.Submit(lease, {{tick * 2 + 2}, DebugCommand::DisplayStep}).result == DebugAdmission::Retired,
				"Long-run retry repeated an acknowledged display tick");
	}
}

void PendingBoundaryAndExactOrder()
{
	Fixture fixture;
	const auto first = fixture.Issue();
	auto firstImage = fixture.Image(first);
	fixture.Submit(first, firstImage);
	const auto second = fixture.Issue();
	auto controllerOwner = std::make_unique<DebugController>(fixture.session, fixture.clock, ManualConfig());
	auto& controller = *controllerOwner;
	const auto lease = controller.Acquire()->id;
	const auto pause = Command(controller, lease, 1, DebugCommand::Pause);
	controller.Poll();
	Require(Status(controller, pause).phase == DebugPhase::Pending &&
				Status(controller, pause).outstanding.size() == 2 && fixture.pool.InUse() == 2,
			"Pause did not pin submitted GPU output and begun boundary");
	auto secondImage = fixture.Image(second);
	firstImage->controller = &controller;
	secondImage->controller = &controller;
	fixture.Submit(second, secondImage);
	controller.Poll();
	Require(Status(controller, pause).phase == DebugPhase::Pending, "Pause fabricated GPU completion");
	firstImage->state.store(CompletionStatus::Complete);
	secondImage->state.store(CompletionStatus::Complete);
	controller.Poll();
	Require(Status(controller, pause).progress == DebugProgress::Paused,
			"Pause did not settle independently of another XR call");
	const auto step1 = Command(controller, lease, 2, DebugCommand::Step, 4);
	controller.Poll();
	const auto step2 = Command(controller, lease, 3, DebugCommand::Step, 4);
	controller.Poll();
	Require(Status(controller, step1).frame->frame == first.id && Status(controller, step2).frame->frame == second.id &&
				Status(controller, step1).progress == DebugProgress::SnapshotReady &&
				!fixture.session->Scheduler().Snapshot().permits && fixture.pool.InUse() == 2,
			"Retained frames were skipped, reordered or admitted more work");
	firstImage->controller = nullptr;
	secondImage->controller = nullptr;
}

void LookaheadStallAndCancellation()
{
	Fixture fixture;
	auto controllerOwner = std::make_unique<DebugController>(fixture.session, fixture.clock, ManualConfig());
	auto& controller = *controllerOwner;
	const auto lease = controller.Acquire()->id;
	Pause(controller, lease);
	const auto initial = Command(controller, lease, 2, DebugCommand::Step);
	controller.Poll();
	const auto first = fixture.Issue();
	FramePrediction second;
	auto waiting = std::async(std::launch::async, [&] { return fixture.session->WaitFrame(second); });
	Eventually([&] { return fixture.session->Scheduler().Snapshot().waitPending; }, "Pipeline wait did not block");
	controller.Poll();
	Require(Status(controller, initial).progress == DebugProgress::PipelineStall &&
				waiting.wait_for(1ms) != std::future_status::ready,
			"Insufficient allowance did not report pending stall");
	Command(controller, lease, 3, DebugCommand::Cancel, 1, initial);
	controller.Poll();
	Require(Status(controller, initial).phase == DebugPhase::Cancelled && fixture.pool.InUse() == 1,
			"Cancellation undid already issued frame retention");
	const auto expanded = Command(controller, lease, 4, DebugCommand::Step, 2);
	controller.Poll();
	Require(waiting.wait_for(1s) == std::future_status::ready && waiting.get() == XR_SUCCESS &&
				fixture.pool.InUse() == 2,
			"Explicit larger allowance did not reserve before unblocking pipeline");
	fixture.Submit(first);
	controller.Poll();
	Require(Status(controller, expanded).frame->frame == first.id && controller.Snapshot().inspected->frame == first.id,
			"Lookahead moved inspection cursor implicitly");
	Require(fixture.session->BeginFrame() == XR_SUCCESS, "Begin lookahead frame");
	fixture.Submit(second);
	const auto next = Command(controller, lease, 5, DebugCommand::Step);
	controller.Poll();
	Require(Status(controller, next).frame->frame == second.id && !fixture.session->Scheduler().Snapshot().permits,
			"Lookahead output did not remain inspectable without another permit");
	const auto unused = Command(controller, lease, 6, DebugCommand::Step);
	controller.Poll();
	const auto boundary = fixture.session->Scheduler().Snapshot().nextAdmission;
	Require(fixture.pool.InUse() == 1 && fixture.session->Scheduler().Snapshot().permits == 1,
			"Unused permit was not reserved");
	Command(controller, lease, 7, DebugCommand::Cancel, 1, unused);
	controller.Poll();
	Require(!fixture.pool.InUse() && !fixture.session->Scheduler().Snapshot().permits &&
				fixture.session->Scheduler().Snapshot().nextAdmission == boundary,
			"Unused cancellation advanced admission or retained budget");
}

void SelectedLayerViewRequests()
{
	Fixture fixture(12, true, true);
	DebugController controller(fixture.session, fixture.clock, ManualConfig());
	const auto lease = controller.Acquire()->id;
	Pause(controller, lease);
	Require(controller.Snapshot().layerViews, "Configured raw capture capability missing");
	std::uint64_t command = 2;
	FramePrediction first;
	const std::array targets{CaptureTarget{CaptureKind::LayerView, 0, 0}, CaptureTarget{CaptureKind::LayerView, 0, 3},
							 CaptureTarget{CaptureKind::LayerView, 1, 0}, CaptureTarget{CaptureKind::LayerView, 0, 9}};
	for (const auto target : targets)
	{
		const auto step = Command(controller, lease, command++, DebugCommand::Step, 1, {}, {}, target);
		controller.Poll();
		const auto pending = Status(controller, step);
		Require(pending.phase == DebugPhase::Pending && pending.captures.size() == 1 &&
					fixture.session->Scheduler().Snapshot().permits == 1,
				"Raw Step reused composed history instead of reserving future source");
		const auto capture = fixture.session->Captures().Read(pending.captures.front());
		Require(capture && capture->target == target && capture->phase == SnapshotPhase::Reserved,
				"Raw Step reserved wrong source target");
		auto prediction = fixture.Issue();
		if (!first.id.value)
			first = prediction;
		auto composed = fixture.Image(prediction);
		composed->state.store(CompletionStatus::Complete);
		std::shared_ptr<Output> raw;
		std::vector<CaptureOutput> outputs;
		if (target.view < 4)
		{
			raw = fixture.Image(prediction, target);
			raw->state.store(CompletionStatus::Complete);
			outputs.push_back({target, raw});
		}
		ProjectionLayerDescription projection;
		projection.views.resize(4);
		auto metadata =
			std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
												std::vector<LayerDescription>{projection, QuadLayerDescription{}});
		const XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO,			nullptr, prediction.displayTime.value.count(),
								  XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 2,		 nullptr};
		Require(fixture.session->EndFrame(
					prediction, &info, metadata, composed, [] { return XR_SUCCESS; }, outputs) == XR_SUCCESS,
				"Submit selected raw output");
		controller.Poll();
		const auto completed = Status(controller, step);
		const auto record = fixture.session->Captures().Read(pending.captures.front());
		Require(completed.frame && completed.frame->frame == prediction.id &&
					completed.capture == pending.captures.front() && record && record->target == target &&
					record->output == raw && record->output != composed,
				"Target matching substituted another view or composed image");
		Require(
			target.view == 9
				? completed.phase == DebugPhase::Failed && record->phase == SnapshotPhase::Unavailable &&
					  completed.result == XR_ERROR_FEATURE_UNSUPPORTED && record->result == XR_ERROR_FEATURE_UNSUPPORTED
				: completed.progress == DebugProgress::SnapshotReady && controller.Snapshot().inspectedTarget == target,
			"Missing raw target did not remain explicitly unavailable");
		if (target == targets.front())
		{

			const auto composedStep = Command(controller, lease, command++, DebugCommand::Step);
			controller.Poll();
			Require(Status(controller, composedStep).progress == DebugProgress::SnapshotReady &&
						Status(controller, composedStep).frame->frame == prediction.id &&
						controller.Snapshot().inspectedTarget == CaptureTarget{},
					"Raw inspection cursor skipped independent composed capture of same frame");
			Require(fixture.session->Scheduler().Snapshot().permits == 0 &&
						fixture.session->Snapshot().lastIssued.id == prediction.id,
					"Switching to retained composed output admitted another host frame");
		}
	}
	fixture.session->Close();
	controller.Close(false);
}

void ReleasedComposedCursorDoesNotRepin()
{
	Fixture fixture(2);
	DebugController controller(fixture.session, fixture.clock, ManualConfig());
	const auto lease = controller.Acquire()->id;
	Pause(controller, lease);
	const auto strict = Command(controller, lease, 2, DebugCommand::StrictAdmission);
	controller.Poll();
	const auto first = fixture.Issue();
	auto output = fixture.Image(first);
	output->state.store(CompletionStatus::Complete);
	fixture.Submit(first, output);
	controller.Poll();
	const auto captured = Status(controller, strict).capture;
	Require(captured.has_value(), "Strict capture missing before composed cursor test");
	const auto inspected = Command(controller, lease, 3, DebugCommand::Step);
	controller.Poll();
	Require(Status(controller, inspected).progress == DebugProgress::SnapshotReady,
			"Initial composed capture not inspected");
	auto reader = fixture.session->Captures().Read(*captured);
	const FrameKey old{fixture.session->Snapshot().generation, first.id};
	Command(controller, lease, 4, DebugCommand::Release, 1, {}, *captured);
	controller.Poll();
	const auto next = Command(controller, lease, 5, DebugCommand::Step);
	controller.Poll();
	const auto queued = Status(controller, next);
	Require(queued.captures.size() == 1 && fixture.session->Captures().Snapshot().captures == queued.captures &&
				!fixture.session->Captures().Find(old) && fixture.pool.InUse() == 2,
			"Composed refresh silently repinned a released frame below its cursor");
	const auto clear = fixture.Issue();
	fixture.Submit(clear);
	controller.Poll();
	Require(Status(controller, next).progress == DebugProgress::Clear && fixture.pool.InUse() == 1,
			"Clear did not return unused admission budget");
	std::uint64_t command = 6;
	Command(controller, lease, command++, DebugCommand::Release, 1, {}, queued.captures.front());
	controller.Poll();
	while (fixture.session->Display().NextTick() <= clear.displayTick)
	{
		const auto step = Command(controller, lease, command++, DebugCommand::DisplayStep);
		controller.Poll();
		Require(Status(controller, step).capture.has_value(), "Display tick capture missing");
		Command(controller, lease, command++, DebugCommand::Release, 1, {}, *Status(controller, step).capture);
		controller.Poll();
	}
	const auto display = fixture.session->Display().Snapshot();
	Require(display.latest && display.latest->kind == PresentationKind::Clear && !display.latest->output &&
				controller.Acknowledge(lease, {command - 1}),
			"Clear display or terminal reader acknowledgement failed");
	output.reset();
	Require(fixture.pool.InUse() == 1, "Explicit snapshot reader lost its retained output");
	reader.reset();
	Require(fixture.pool.InUse() == 0 && fixture.session->Captures().Snapshot().captures.empty(),
			"Released composed snapshot leaked hidden retention after final reader");
}

void RollingStepsAndPins()
{
	Fixture fixture(4);
	DebugController controller(fixture.session, fixture.clock, ManualConfig());
	const auto lease = controller.Acquire()->id;
	Pause(controller, lease);
	Require(controller.Acknowledge(lease, {1}), "Acknowledge initial pause");
	std::uint64_t command = 2;
	CaptureId pinned;
	for (int index = 0; index < 200; ++index)
	{
		DebugRequest request{{command}, DebugCommand::Step};
		request.advanceDisplay = true;
		const auto admitted = controller.Submit(lease, request);
		Require(admitted.result == DebugAdmission::Accepted, "Admit rolling step");
		const auto step = admitted.operation;
		controller.Poll();
		Require(Status(controller, step).phase != DebugPhase::Failed, "Rolling step exhausted capacity");
		const auto frame = fixture.Issue();
		auto output = fixture.Image(frame);
		output->state = CompletionStatus::Complete;
		fixture.Submit(frame, std::move(output));
		controller.Poll();
		const auto completed = Status(controller, step);
		Require(completed.phase == DebugPhase::Complete, "Rolling step did not complete");
		Require(controller.Acknowledge(lease, {command++}), "Acknowledge rolling step");
		if (!index)
		{
			Require(completed.capture.has_value(), "Completed step omitted capture");
			pinned = *completed.capture;
			const auto pin = Command(controller, lease, command, DebugCommand::PinSnapshot, 1, {}, pinned);
			controller.Poll();
			Require(Status(controller, pin).phase == DebugPhase::Complete, "Pin command failed");
			Require(controller.Acknowledge(lease, {command++}), "Acknowledge pin");
		}
		const auto retained = fixture.session->Captures().Read(pinned);
		Require(retained && retained->pinned, "Rolling stepping evicted pinned snapshot");
	}
	const auto unpin = Command(controller, lease, command, DebugCommand::UnpinSnapshot, 1, {}, pinned);
	controller.Poll();
	Require(Status(controller, unpin).phase == DebugPhase::Complete &&
		!fixture.session->Captures().Read(pinned)->pinned, "Unpin command failed");
}

void CapacityAndUnsupported()
{
	Fixture fixture(1);
	auto controllerOwner = std::make_unique<DebugController>(fixture.session, fixture.clock, ManualConfig());
	auto& controller = *controllerOwner;
	const auto lease = controller.Acquire()->id;
	Pause(controller, lease);
	const auto step = Command(controller, lease, 2, DebugCommand::Step, 2);
	controller.Poll();
	Require(Status(controller, step).progress == DebugProgress::RetentionFailure && fixture.pool.InUse() == 0 &&
				!fixture.session->Scheduler().Snapshot().permits,
			"Partial lookahead reservation leaked or granted work");
	DebugRequest raw{{3}, DebugCommand::Step};
	raw.target = {CaptureKind::LayerView, 0, 0};
	const auto rejected = controller.Submit(lease, raw);
	controller.Poll();
	Require(Status(controller, rejected.operation).progress == DebugProgress::Unsupported,
			"Raw layer capture falsely advertised as implemented");
	Fixture absent(1, false);
	auto disabledOwner = std::make_unique<DebugController>(absent.session, absent.clock, ManualConfig());
	auto& disabled = *disabledOwner;
	Require(!disabled.Snapshot().composedEyes, "Unconfigured native capture capability advertised");
	Fixture boundary(1);
	boundary.Issue();
	boundary.Issue(false);
	auto crowdedOwner = std::make_unique<DebugController>(boundary.session, boundary.clock, ManualConfig());
	auto& crowded = *crowdedOwner;
	const auto owner = crowded.Acquire()->id;
	const auto pause = Command(crowded, owner, 1, DebugCommand::Pause);
	crowded.Poll();
	Require(Status(crowded, pause).progress == DebugProgress::RetentionFailure && !boundary.pool.InUse() &&
				boundary.session->Scheduler().Snapshot().paused && !boundary.session->Scheduler().Snapshot().permits,
			"Boundary retention failure reopened admission or leaked partial reservations");
}

void ExpiryAndSharedResume()
{
	Fixture fixture;
	auto controllerOwner = std::make_unique<DebugController>(fixture.session, fixture.clock, ManualConfig());
	auto& controller = *controllerOwner;
	const auto lease = controller.Acquire()->id;
	Pause(controller, lease);
	const auto strict = Command(controller, lease, 2, DebugCommand::StrictAdmission);
	controller.Poll();
	const auto frame = fixture.Issue();
	auto output = fixture.Image(frame);
	fixture.Submit(frame, output);
	controller.Poll();
	Require(Status(controller, strict).progress == DebugProgress::Issued, "Strict command did not report issued ID");
	const auto inspection = Command(controller, lease, 3, DebugCommand::Step);
	controller.Poll();
	Require(Status(controller, inspection).progress == DebugProgress::GpuPending, "Pending fence was not reported");
	const auto capture = fixture.session->Captures().Find({fixture.session->Snapshot().generation, frame.id});
	Require(capture.has_value(), "Missing pending capture before expiry");
	auto reader = fixture.session->Captures().Read(*capture);
	Require(controller.Acknowledge(lease, {2}), "Pre-expiry terminal acknowledgement failed");
	fixture.clock->time.store(2000000000);
	controller.Poll();
	Require(Status(controller, inspection).phase == DebugPhase::Interrupted &&
				Status(controller, inspection).progress == DebugProgress::LeaseExpired &&
				!fixture.session->Scheduler().Snapshot().paused && !fixture.session->Display().Paused() &&
				!controller.Renew(lease),
			"Lease expiry did not interrupt and restore original execution mode");
	Require(!fixture.session->Captures().Read(*capture) && reader && reader->phase == SnapshotPhase::GpuPending &&
				reader->output == output && output->PollCompletion() == CompletionStatus::Pending &&
				fixture.pool.InUse() == 1,
			"Expiry fabricated completion or lost retained reader");
	output->state.store(CompletionStatus::Complete);
	controller.Poll();
	Require(fixture.session->Captures().Snapshot().retiring == 0 && fixture.pool.InUse() == 1,
			"Abandoned capture did not retire safely after completion");
	const auto next = fixture.Issue();
	Require(next.displayTick.value <= frame.displayTick.value + 2,
			"Time spent inspecting caused deterministic catch-up advancement");
	fixture.Submit(next);
	const auto replacement = controller.Acquire();
	Require(replacement && replacement->id != lease && controller.Renew(replacement->id),
			"Expired lease not replaceable");
	Require(controller.Acknowledge(lease, {3}), "Expired lease terminal records could not be reclaimed");
	Require(controller.Submit(lease, {{3}, DebugCommand::Step}).result == DebugAdmission::LeaseRequired,
			"Expired acknowledged retry acquired a new permit");
	fixture.session->Close();
	output.reset();
	Require(fixture.pool.InUse() == 1, "Explicit reader did not survive lease expiry and session close");
	reader.reset();
	Require(fixture.pool.InUse() == 0, "Expired final reader leaked native reservation");
}

void SubmittedWaitDiscardAndRelease()
{
	Fixture fixture;
	auto controllerOwner = std::make_unique<DebugController>(fixture.session, fixture.clock, ManualConfig());
	auto& controller = *controllerOwner;
	const auto lease = controller.Acquire()->id;
	Pause(controller, lease);
	const auto strict = Command(controller, lease, 2, DebugCommand::StrictAdmission);
	controller.Poll();
	const auto capture = Status(controller, strict).captures.front();
	const auto earlyRelease = Command(controller, lease, 3, DebugCommand::Release, 1, {}, capture);
	controller.Poll();
	Require(Status(controller, earlyRelease).progress == DebugProgress::Busy && fixture.pool.InUse() == 1 &&
				fixture.session->Scheduler().Snapshot().permits == 1,
			"Release stranded reserved queued capture");
	const auto frame = fixture.Issue();
	auto output = fixture.Image(frame);
	fixture.Submit(frame, output);
	controller.Poll();
	const auto pending = Command(controller, lease, 4, DebugCommand::Step, 4);
	controller.Poll();
	Require(Status(controller, pending).progress == DebugProgress::GpuPending && fixture.pool.InUse() == 1 &&
				!fixture.session->Scheduler().Snapshot().permits,
			"Submitted GPU wait granted needless lookahead");
	Command(controller, lease, 5, DebugCommand::Release, 1, {}, capture);
	controller.Poll();
	Require(Status(controller, pending).phase == DebugPhase::Cancelled,
			"Releasing active inspection stranded operation");
	output->state.store(CompletionStatus::Failed);
	const auto failure = Command(controller, lease, 6, DebugCommand::Pause);
	controller.Poll();
	controller.Poll();
	Require(Status(controller, failure).phase == DebugPhase::Failed && !Status(controller, failure).outstanding.empty(),
			"Failed GPU boundary falsely acknowledged settled pause");
	fixture.session->Close();

	Fixture discarded;
	auto inspectorOwner = std::make_unique<DebugController>(discarded.session, discarded.clock, ManualConfig());
	auto& inspector = *inspectorOwner;
	const auto owner = inspector.Acquire()->id;
	Pause(inspector, owner);
	const auto one = Command(inspector, owner, 2, DebugCommand::StrictAdmission);
	inspector.Poll();
	const auto first = discarded.Issue();
	inspector.Poll();
	const auto oldCapture = *Status(inspector, one).capture;
	Command(inspector, owner, 3, DebugCommand::StrictAdmission);
	inspector.Poll();
	FramePrediction second;
	Require(discarded.session->WaitFrame(second) == XR_SUCCESS && discarded.session->BeginFrame() == XR_FRAME_DISCARDED,
			"Discard old begun frame");
	discarded.Submit(second);
	inspector.Poll();
	const auto next = Command(inspector, owner, 4, DebugCommand::Step);
	inspector.Poll();
	const auto selected = Status(inspector, next);
	Require(selected.frame->frame == second.id && selected.frame->frame != first.id &&
				selected.progress == DebugProgress::Clear &&
				std::find(selected.diagnostics.begin(), selected.diagnostics.end(), oldCapture) !=
					selected.diagnostics.end() &&
				discarded.session->Captures().Read(oldCapture)->phase == SnapshotPhase::Discarded,
			"Discarded diagnostic substituted for next submitted frame");
}

void ExpiryOwnershipAndReentrantAcknowledgement()
{
	Fixture fixture;
	const auto old = fixture.Issue();
	auto externalImage = fixture.Image(old);
	externalImage->state.store(CompletionStatus::Complete);
	fixture.Submit(old, externalImage);
	const auto external = fixture.session->Captures().Pin(
		fixture.session->History().Find({fixture.session->Snapshot().generation, old.id}), {}, externalImage);
	Require(external.result == SnapshotAdmission::Accepted, "Externally pin submitted output");
	auto controllerOwner = std::make_unique<DebugController>(fixture.session, fixture.clock, ManualConfig());
	auto& controller = *controllerOwner;
	const auto lease = controller.Acquire()->id;
	Pause(controller, lease);
	const auto strict = Command(controller, lease, 2, DebugCommand::StrictAdmission);
	controller.Poll();
	const auto next = fixture.Issue();
	auto ready = fixture.Image(next);
	ready->state.store(CompletionStatus::Complete);
	fixture.Submit(next, ready);
	controller.Poll();
	const auto owned = *Status(controller, strict).capture;
	Require(controller.Acknowledge(lease, {2}), "Ack must not lose lease capture ownership");
	fixture.clock->time.store(2000000000);
	controller.Poll();
	Require(!fixture.session->Captures().Read(owned) && fixture.session->Captures().Read(external.id),
			"Expiry lost acknowledged ownership or retired externally owned boundary capture");
	controller.Close(false);
	controller.Close(true);
	Require(!controller.Acquire() && !controller.Renew(lease) &&
				controller.Submit(lease, {{20}, DebugCommand::Pause}).result == DebugAdmission::Closed,
			"Closed controller accepted new control work");

	Fixture reentrant;
	auto debugOwner = std::make_unique<DebugController>(reentrant.session, reentrant.clock, ManualConfig());
	auto& debug = *debugOwner;
	const auto owner = debug.Acquire()->id;
	Pause(debug, owner);
	const auto admission = Command(debug, owner, 2, DebugCommand::StrictAdmission);
	debug.Poll();
	const auto first = reentrant.Issue();
	auto image = reentrant.Image(first);
	image->state.store(CompletionStatus::Complete);
	bool destroyed = false;
	image->onDestroy = [&]
	{
		debug.Snapshot();
		destroyed = true;
	};
	reentrant.Submit(first, image);
	debug.Poll();
	const auto composed = *Status(debug, admission).capture;
	Command(debug, owner, 3, DebugCommand::DisplayStep);
	debug.Poll();
	const auto display = Command(debug, owner, 4, DebugCommand::DisplayStep);
	debug.Poll();
	const auto displayCapture = *Status(debug, display).capture;
	Command(debug, owner, 5, DebugCommand::Release, 1, {}, composed);
	Command(debug, owner, 6, DebugCommand::Release, 1, {}, displayCapture);
	debug.Poll();
	Command(debug, owner, 7, DebugCommand::StrictAdmission);
	debug.Poll();
	const auto clear = reentrant.Issue();
	reentrant.Submit(clear);
	debug.Poll();
	Command(debug, owner, 8, DebugCommand::DisplayStep);
	debug.Poll();
	image.reset();
	Require(!destroyed, "Display command failed to retain output reader");
	Require(debug.Acknowledge(owner, {4}) && destroyed, "Terminal record destruction deadlocked status reentry");
	const auto pending = Command(debug, owner, 9, DebugCommand::StrictAdmission);
	debug.Poll();
	debug.Close(false);
	Require(reentrant.session->Scheduler().Snapshot().paused && reentrant.session->Display().Paused() &&
				!reentrant.session->Scheduler().Snapshot().permits &&
				Status(debug, pending).phase == DebugPhase::Interrupted,
			"Parent Close(false) reopened pacing or left unused permits");
}

class BlockingClock final : public Clock
{
  public:
	HostTime Now() const override
	{
		std::unique_lock lock(mutex);
		if (armed)
		{
			armed = false;
			entered = true;
			changed.notify_all();
			changed.wait_for(lock, 2s, [&] { return released; });
		}
		return HostTime{Duration{0}};
	}
	void WaitUntil(std::condition_variable& condition, std::unique_lock<std::mutex>& lock, HostTime) override
	{
		condition.wait_for(lock, 1ms);
	}
	void Arm()
	{
		std::lock_guard lock(mutex);
		armed = true;
		entered = released = false;
	}
	bool Entered() const
	{
		std::lock_guard lock(mutex);
		return entered;
	}
	void Release()
	{
		std::lock_guard lock(mutex);
		released = true;
		changed.notify_all();
	}

  private:
	mutable std::mutex mutex;
	mutable std::condition_variable changed;
	mutable bool armed = false, entered = false, released = false;
};

void BoundLeaseGeneration()
{
	auto clock = std::make_shared<BlockingClock>();
	auto instance = std::make_shared<Instance>(reinterpret_cast<XrInstance>(180), std::span<const char* const>{},
											   XR_MAKE_VERSION(1, 0, 27));
	auto session = std::make_shared<Session>(reinterpret_cast<XrSession>(181), instance, clock);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS,
			"Begin generation-bound lease session");
	DebugControllerConfig config;
	config.startWorker = false;
	DebugController controller(session, clock, config);
	const auto first = session->Snapshot().generation;
	clock->Arm();
	auto acquisition = std::async(std::launch::async, [&] { return controller.Acquire(first); });
	Eventually([&] { return clock->Entered(); }, "Bound Acquire did not reach controlled clock");
	std::atomic<bool> ending{false};
	auto restart = std::async(
		std::launch::async,
		[&]
		{
			Require(session->RequestExit(timing.epoch) == XR_SUCCESS, "Request restart during bound acquisition");
			ending.store(true);
			Require(session->End(timing.epoch) == XR_SUCCESS && session->Ready(timing.epoch) == XR_SUCCESS &&
						session->Begin(begin, timing) == XR_SUCCESS,
					"Restart generation-bound lease session");
		});
	Eventually([&] { return ending.load(); }, "Concurrent restart did not attempt End");
	Require(restart.wait_for(20ms) == std::future_status::timeout,
			"Lifecycle restart passed generation-bound lease mutation");
	clock->Release();
	const auto lease = acquisition.get();
	Require(lease.has_value(), "Bound lease acquisition failed before serialized restart");
	restart.get();
	const auto second = session->Snapshot().generation;
	Require(second > first && !controller.Acquire(first) && !controller.Renew(lease->id, first) &&
				!controller.Relinquish(lease->id, first) && !controller.Acknowledge(lease->id, {1}, nullptr, first),
			"Old generation changed lease or retired command range after restart");
	Require(controller.Renew(lease->id, second), "Current-generation lease renewal failed");
	DebugRequest pause{{1}, DebugCommand::Pause};
	pause.generation = second;
	const auto admitted = controller.Submit(lease->id, pause);
	Require(admitted.result == DebugAdmission::Accepted, "Stale acknowledgement changed current command ledger");
	controller.Poll();
	Require(controller.Read(admitted.operation)->phase == DebugPhase::Complete &&
				!controller.Acknowledge(lease->id, {1}, nullptr, first) && controller.Read(admitted.operation),
			"Stale acknowledgement retired a current operation");
	Require(session->RequestExit(timing.epoch) == XR_SUCCESS && session->End(timing.epoch) == XR_SUCCESS,
			"End session before terminal acknowledgement");
	std::vector<DebugOperation> retired;
	Require(controller.Acknowledge(lease->id, {1}, &retired, second) && retired.size() == 1 &&
				!controller.Read(admitted.operation) && controller.Relinquish(lease->id, second),
			"Matching-generation terminal Ack/lease release required a running session");
	controller.Close(false);
	session->Close();
}

void WorkerAndLifetime()
{
	auto instance = std::make_shared<Instance>(reinterpret_cast<XrInstance>(200), std::span<const char* const>{},
											   XR_MAKE_VERSION(1, 0, 27));
	auto session = std::make_shared<Session>(reinterpret_cast<XrSession>(201), instance);
	FramePool pool(4);
	Require(session->ConfigureCapturePool(pool) == XR_SUCCESS, "Configure worker capture pool");
	FrameSchedulerConfig timing;
	timing.period = 1ms;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS,
			"Begin real worker session");
	{
		auto controllerOwner = std::make_unique<DebugController>(session);
		auto& controller = *controllerOwner;
		const auto lease = controller.Acquire()->id;
		const auto pause = Command(controller, lease, 1, DebugCommand::Pause);
		Eventually([&] { return Status(controller, pause).phase == DebugPhase::Complete; },
				   "Worker did not settle pause");
		const auto strict = Command(controller, lease, 2, DebugCommand::StrictAdmission);
		Eventually([&] { return session->Scheduler().Snapshot().permits == 1; }, "Worker did not grant strict permit");
		FramePrediction frame;
		Require(session->WaitFrame(frame) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS,
				"Issue real lifecycle frame");
		auto output = std::make_shared<Output>();
		output->reservation = session->Captures().ProducerReservation({session->Snapshot().generation, frame.id});
		Require(pool.Consume(output->reservation), "Consume lifecycle reservation");
		auto metadata =
			std::make_shared<const FrameLayers>(frame.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
												std::vector<LayerDescription>{ProjectionLayerDescription{}});
		XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,			 nullptr, frame.displayTime.value.count(),
						   XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		  nullptr};
		Require(session->EndFrame(frame, &end, metadata, output, [] { return XR_SUCCESS; }) == XR_SUCCESS,
				"Submit lifecycle frame");
		Eventually([&] { return Status(controller, strict).phase == DebugPhase::Complete; },
				   "Strict issue not observed");
		const auto step = Command(controller, lease, 3, DebugCommand::Step);
		Eventually([&] { return Status(controller, step).progress == DebugProgress::GpuPending; }, "Fence not pending");
		output->state.store(CompletionStatus::Complete);
		Eventually([&] { return Status(controller, step).progress == DebugProgress::SnapshotReady; },
				   "Control worker needed another XR call to settle completion");
	}
	Require(!session->Scheduler().Snapshot().paused && !session->Display().Paused(),
			"Controller destruction failed to restore live session pacing");
	{
		auto controllerOwner = std::make_unique<DebugController>(session);
		auto& controller = *controllerOwner;
		const auto lease = controller.Acquire()->id;
		const auto pause = Command(controller, lease, 1, DebugCommand::Pause);
		Eventually([&] { return Status(controller, pause).phase == DebugPhase::Complete; }, "Second lifecycle pause");
		const auto strict = Command(controller, lease, 2, DebugCommand::StrictAdmission);
		Eventually([&] { return session->Scheduler().Snapshot().permits == 1; }, "Second lifecycle permit");
		session->Close();
		Eventually([&] { return Status(controller, strict).progress == DebugProgress::SessionEnded; },
				   "Parent close did not interrupt controller operation");
	}
	Require(pool.InUse() == 0, "Controller/session teardown retained unused or completed reservations");
}
void BundleReservationRollback()
{
	Fixture fixture(3, true, true);
	fixture.session->Scheduler().Pause();
	const std::array targets{CaptureTarget{CaptureKind::LayerView, 0, 0},
		CaptureTarget{CaptureKind::LayerView, 0, 1}};
	const auto rejected = fixture.session->ReserveCaptureBundle(targets);
	Require(rejected.result == SnapshotAdmission::Capacity && rejected.captures.empty(), "Bundle must fail atomically");
	Require(fixture.pool.InUse() == 0 && fixture.session->Captures().Snapshot().heldSlots == 0,
		"Failed bundle leaked reservations");
	Require(fixture.session->Scheduler().Snapshot().permits == 0, "Failed bundle admitted work");
	const auto accepted = fixture.session->ReserveCapture();
	Require(accepted.result == SnapshotAdmission::Accepted, "Rollback did not restore capacity");

	Fixture room(8, true, true);
	room.session->Captures().SetCapacity(3);
	std::array<CaptureId, 2> previous;
	for (auto& id : previous)
	{
		const auto frame = room.Issue();
		room.Submit(frame);
		const auto pinned = room.session->Captures().Pin(
			room.session->History().Find({room.session->Snapshot().generation, frame.id}));
		Require(pinned.result == SnapshotAdmission::Accepted, "Retain cleared frame before bundle");
		id = pinned.id;
	}
	room.session->Scheduler().Pause();
	const auto bundle = room.session->ReserveCaptureBundle(targets);
	Require(bundle.result == SnapshotAdmission::Accepted && bundle.captures.size() == 2 &&
		!room.session->Captures().Read(previous[0]) && room.session->Captures().Read(previous[1]),
		"Bundle reclamation did not make room for all snapshot slots");
}
} // namespace

int main()
{
	unsigned failures = 0;
	const auto run = [&](const char* name, auto test)
	{
		try
		{
			test();
			std::cout << name << " passed\n";
		}
		catch (const std::exception& error)
		{
			std::cerr << name << ": " << error.what() << '\n';
			++failures;
		}
	};
	run("Bundle reservation rollback", BundleReservationRollback);
	run("Display step allocation recovery", DisplayStepAllocationRecovery);
	run("Realtime recording budget and lease recovery", RealtimeRecordingBudget);
	run("Idempotence and reclamation", IdempotenceAndReclamation);
	run("Long run acknowledgement", LongRunAcknowledgement);
	run("Pending boundary and exact order", PendingBoundaryAndExactOrder);
	run("Lookahead stall and cancellation", LookaheadStallAndCancellation);
	run("Rolling steps and pins", RollingStepsAndPins);
	run("Capacity and unsupported targets", CapacityAndUnsupported);
	run("Selected layer-view requests", SelectedLayerViewRequests);
	run("Released composed cursor does not repin", ReleasedComposedCursorDoesNotRepin);
	run("Expiry and shared resume", ExpiryAndSharedResume);
	run("Submitted wait, discard and release", SubmittedWaitDiscardAndRelease);
	run("Expiry ownership and reentrant acknowledgement", ExpiryOwnershipAndReentrantAcknowledgement);
	run("Generation-bound lease and acknowledgement", BoundLeaseGeneration);
	run("Worker and lifetime", WorkerAndLifetime);
	return failures ? 1 : 0;
}
