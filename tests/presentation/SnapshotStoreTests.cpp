#include "graphics/SnapshotBudget.h"
#include "../TestSupport.h"
#include "presentation/SnapshotStore.h"

#include <array>
#include <atomic>
#include <future>
#include <iostream>
#include <limits>

using namespace simxr;

namespace
{
class Output final : public FrameOutput
{
  public:
	CompletionStatus PollCompletion() const noexcept override
	{
		if (store)
			store->Read(id);
		return status.load();
	}
	~Output() override
	{
		if (store && destroyed)
			*destroyed = store->Snapshot().retiring == 0;
	}
	std::atomic<CompletionStatus> status{CompletionStatus::Pending};
	SnapshotStore* store = nullptr;
	CaptureId id;
	bool* destroyed = nullptr;
};

class RetainedSource final : public RotationalSource
{
  public:
	CompletionStatus PollCompletion() const noexcept override
	{
		return CompletionStatus::Complete;
	}
	PoseSample SubmittedHead() const noexcept override
	{
		return {};
	}
	XrResult Reproject(const PoseSample&, std::shared_ptr<const FrameOutput>&) const override
	{
		return XR_ERROR_FEATURE_UNSUPPORTED;
	}
};

struct Records
{
	std::shared_ptr<const FrameRecord> issued;
	std::shared_ptr<const FrameRecord> cancelled;
	std::shared_ptr<const FrameRecord> begun;
	std::shared_ptr<const FrameRecord> submitted;
};

Records Frame(SessionGeneration generation, std::uint64_t frame, std::uint32_t layers = 1)
{
	FrameRecord issued{{generation, {frame}},
					   {{frame}, {frame}, {Duration{100 + static_cast<std::int64_t>(frame * 10)}}, Duration{10}}};
	auto cancelled = issued;
	cancelled.stage = FrameStage::Cancelled;
	auto begun = issued;
	begun.stage = FrameStage::Begun;
	auto submitted = begun;
	submitted.stage = FrameStage::Submitted;
	submitted.submission.emplace();
	submitted.submission->layerCount = layers;
	submitted.submission->displayTime = issued.prediction.displayTime;
	return {std::make_shared<const FrameRecord>(issued), std::make_shared<const FrameRecord>(cancelled),
			std::make_shared<const FrameRecord>(begun), std::make_shared<const FrameRecord>(submitted)};
}

void CaptureHistory()
{
	FrameHistory history;
	SnapshotStore store(4, &history);
	store.Start({1});
	const auto records = Frame({1}, 1);
	auto image = std::make_shared<Output>();
	std::weak_ptr<Output> lifetime = image;
	const auto capture = store.Pin(records.submitted, {}, image);
	store.Poll();
	Require(history.Diagnostics().events.empty(), "Pending capture claimed a terminal result");
	image->status = CompletionStatus::Complete;
	store.Poll();
	store.Poll();
	Require(store.Release(capture.id), "Release traced capture");
	image.reset();
	const auto ready = history.Diagnostics().events;
	Require(ready.size() == 1 && ready[0].kind == FrameTraceKind::CaptureReady &&
			ready[0].frame == records.submitted->key && ready[0].related == capture.id.value && lifetime.expired(),
			"Capture completion was duplicated, lost after release, or retained the GPU image");
	const auto clear = store.Pin(Frame({1}, 2, 0).submitted);
	const auto unavailable = store.Pin(Frame({1}, 3).submitted);
	const auto cancelled = store.Reserve();
	store.Stop();
	const auto terminal = history.Diagnostics().events;
	Require(terminal.size() == 4 && terminal[1].related == clear.id.value &&
			terminal[1].Detail().starts_with("phase=clear;") &&
			terminal[2].related == unavailable.id.value && terminal[2].kind == FrameTraceKind::CaptureFailed &&
			terminal[2].result == XR_ERROR_FEATURE_UNSUPPORTED && terminal[3].related == cancelled.id.value &&
			terminal[3].frame == FrameKey{{1}, {}} && terminal[3].Detail().starts_with("phase=cancelled;"),
			"Clear, unavailable or unissued cancellation lost its capture identity");
	store.Release(clear.id);
	store.Release(unavailable.id);
	store.Release(cancelled.id);
	store.Start({2});
	auto failedImage = std::make_shared<Output>();
	const auto failed = store.Pin(Frame({2}, 1).submitted, {}, failedImage);
	failedImage->status = CompletionStatus::Failed;
	store.Poll();
	const auto failure = history.Diagnostics().events.back();
	Require(failure.related == failed.id.value && failure.frame == FrameKey{{2}, {1}} &&
			failure.kind == FrameTraceKind::CaptureFailed && failure.result == XR_ERROR_RUNTIME_FAILURE,
			"GPU failure or restarted capture correlation was fabricated");
}

void RetentionAndCompletion()
{
	SnapshotStore store(2);
	Require(store.Reserve().result == SnapshotAdmission::Closed, "Unstarted store admitted a capture");
	store.Start({1});
	const auto first = store.Reserve();
	const auto second = store.Reserve();
	Require(first.result == SnapshotAdmission::Accepted && second.result == SnapshotAdmission::Accepted &&
				first.id != second.id && store.Reserve().result == SnapshotAdmission::Capacity,
			"Snapshot capacity or identity failed");
	const auto records = Frame({1}, 1);
	Require(!store.Bind(first.id, records.issued, records.begun) &&
				store.Bind(first.id, records.issued, records.cancelled) &&
				!store.Bind(first.id, records.issued, records.cancelled),
			"Snapshot binding did not validate atomic admission");
	const auto issuedReader = store.Read(first.id);
	store.Update(records.begun);
	Require(store.Read(first.id)->phase == SnapshotPhase::Begun && issuedReader->phase == SnapshotPhase::Issued,
			"Begin changed an immutable reader");
	auto image = std::make_shared<Output>();
	image->store = &store;
	image->id = first.id;
	const std::array outputs{CaptureOutput{{}, image}};
	store.Update(records.submitted, outputs);
	auto pendingReader = store.Read(first.id);
	store.Poll();
	Require(pendingReader->phase == SnapshotPhase::GpuPending &&
				store.Read(first.id)->phase == SnapshotPhase::GpuPending,
			"Observation fabricated GPU completion");
	image->status.store(CompletionStatus::Complete);
	store.Poll();
	Require(store.Read(first.id)->phase == SnapshotPhase::Ready && pendingReader->phase == SnapshotPhase::GpuPending &&
				store.Read(first.id)->metadata == records.submitted,
			"GPU readiness or immutable metadata failed");
	Require(store.Release(first.id) && !store.Read(first.id) && !store.Release(first.id),
			"Explicit release retained public lookup");
	Require(store.Reserve().result == SnapshotAdmission::Capacity,
			"Released capture reused a slot still held by readers");
	pendingReader.reset();
	Require(store.Snapshot().heldSlots == 2, "A second immutable reader did not retain its slot");
	Require(store.Release(second.id), "Unbound reservation release failed");
	Require(store.Snapshot().heldSlots == 1 && store.Reserve().result == SnapshotAdmission::Accepted,
			"Releasing another capture did not free its own slot");
}

void PendingRetirementAndReentry()
{
	SnapshotStore store(1);
	store.Start({2});
	const auto capture = store.Reserve();
	const auto records = Frame({2}, 1);
	Require(store.Bind(capture.id, records.issued, records.cancelled), "Pending capture bind failed");
	bool destroyed = false;
	auto output = std::make_shared<Output>();
	output->store = &store;
	output->id = capture.id;
	output->destroyed = &destroyed;
	std::weak_ptr<Output> lifetime = output;
	{
		const std::array outputs{CaptureOutput{{}, output}};
		store.Update(records.submitted, outputs);
	}
	Require(store.Release(capture.id) && !store.Read(capture.id) && store.Snapshot().retiring == 1,
			"Pending released producer was not retired");
	store.Poll();
	Require(store.Reserve().result == SnapshotAdmission::Capacity && !destroyed, "Pending producer was freed early");
	output->status.store(CompletionStatus::Complete);
	output.reset();
	store.Poll();
	Require(lifetime.expired() && destroyed && store.Snapshot().heldSlots == 0,
			"Completed retirement did not release owners outside the store mutex");
	const auto failed = store.Reserve();
	const auto next = Frame({2}, 2);
	Require(store.Bind(failed.id, next.issued, next.cancelled), "Failed capture bind failed");
	auto failure = std::make_shared<Output>();
	failure->status.store(CompletionStatus::Failed);
	const std::array outputs{CaptureOutput{{}, failure}};
	store.Update(next.submitted, outputs);
	store.Poll();
	Require(store.Read(failed.id)->phase == SnapshotPhase::Failed, "Producer failure became a successful snapshot");
	store.Stop();
	Require(store.Reserve().result == SnapshotAdmission::Closed,
			"Stopped full store reported capacity instead of closure");
}

void TargetsAndGenerations()
{
	SnapshotStore store(8);
	store.Start({3});
	const auto eyes = store.Reserve();
	const CaptureTarget raw{CaptureKind::LayerView, 2, 3};
	const auto view = store.Reserve(raw);
	const auto missing = store.Reserve({CaptureKind::LayerView, 2, 2});
	const CaptureTarget invalidProjection{CaptureKind::LayerView, 2, 4}, invalidQuad{CaptureKind::LayerView, 0, 1};
	const auto invalid = store.Reserve(invalidProjection), invalidQuadCapture = store.Reserve(invalidQuad);
	auto records = Frame({3}, 1);
	auto described = *records.submitted;
	ProjectionLayerDescription four;
	four.views.resize(4);
	described.submission->layerCount = 3;
	described.submission->layers = std::make_shared<const FrameLayers>(
		described.prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		std::vector<LayerDescription>{QuadLayerDescription{}, ProjectionLayerDescription{}, four});
	records.submitted = std::make_shared<const FrameRecord>(std::move(described));
	for (const auto id : {eyes.id, view.id, missing.id, invalid.id, invalidQuadCapture.id})
		Require(store.Bind(id, records.issued, records.cancelled), "Target capture bind failed");
	auto composed = std::make_shared<Output>();
	auto layer = std::make_shared<Output>();
	composed->status.store(CompletionStatus::Complete);
	layer->status.store(CompletionStatus::Complete);
	const std::array outputs{CaptureOutput{{}, composed}, CaptureOutput{raw, layer},
							 CaptureOutput{invalidProjection, layer}, CaptureOutput{invalidQuad, layer}};
	store.Update(records.submitted, outputs);
	store.Poll();
	Require(store.Read(eyes.id)->output == composed && store.Read(view.id)->output == layer &&
				store.Read(missing.id)->phase == SnapshotPhase::Unavailable && !store.Read(missing.id)->output &&
				store.Read(missing.id)->result == XR_ERROR_FEATURE_UNSUPPORTED &&
				store.Read(invalid.id)->phase == SnapshotPhase::Unavailable && !store.Read(invalid.id)->output &&
				store.Read(invalidQuadCapture.id)->phase == SnapshotPhase::Unavailable &&
				!store.Read(invalidQuadCapture.id)->output,
			"A missing layer/view silently substituted the composed output");
	const auto clear = store.Reserve();
	const auto empty = Frame({3}, 2, 0);
	Require(store.Bind(clear.id, empty.issued, empty.cancelled), "Clear bind failed");
	store.Update(empty.submitted);
	Require(store.Read(clear.id)->phase == SnapshotPhase::Clear && !store.Read(clear.id)->output,
			"Empty submission substituted an earlier image");
	const auto pending = store.Reserve();
	const auto boundary = Frame({3}, 3);
	Require(store.Bind(pending.id, boundary.issued, boundary.cancelled), "Boundary capture bind failed");
	store.Update(boundary.begun);
	store.Stop();
	Require(store.Read(pending.id)->phase == SnapshotPhase::Cancelled &&
				store.Read(pending.id)->metadata == boundary.cancelled,
			"Teardown failed to publish preallocated cancellation metadata");
	store.Start({4});
	Require(store.Read(eyes.id)->phase == SnapshotPhase::Ready && store.Read(eyes.id)->frame == records.issued->key,
			"Restart invalidated a retained old-generation snapshot");
	const auto newCapture = store.Reserve();
	Require(!store.Bind(newCapture.id, records.issued, records.cancelled), "Old frame bound into a new generation");
	Require(store.Reserve({CaptureKind::ComposedEyes, 1, 0}).result == SnapshotAdmission::Invalid,
			"Invalid composed target accepted");
}

void DepthRequiresSubmittedSource()
{
	SnapshotStore store(4);
	store.Start({3});
	const CaptureTarget depth{CaptureKind::LayerDepth, 0, 0};
	const CaptureTarget missing{CaptureKind::LayerDepthLinear, 0, 1};
	const auto retained = store.Reserve(depth), absent = store.Reserve(missing);
	auto records = Frame({3}, 1);
	auto described = *records.submitted;
	ProjectionLayerDescription projection;
	projection.views.resize(2);
	projection.views[0].depth = LayerDepthDescription{};
	described.submission->layers = std::make_shared<const FrameLayers>(
		described.prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		std::vector<LayerDescription>{projection});
	records.submitted = std::make_shared<const FrameRecord>(std::move(described));
	Require(store.Bind(retained.id, records.issued, records.cancelled) &&
		store.Bind(absent.id, records.issued, records.cancelled), "Bind exact depth targets");
	auto output = std::make_shared<Output>();
	output->status.store(CompletionStatus::Complete);
	const std::array outputs{CaptureOutput{depth, output}, CaptureOutput{missing, output}};
	store.Update(records.submitted, outputs);
	store.Poll();
	Require(store.Read(retained.id)->phase == SnapshotPhase::Ready && store.Read(retained.id)->output == output &&
		store.Read(absent.id)->phase == SnapshotPhase::Unavailable && !store.Read(absent.id)->output,
		"Depth target accepted pixels without the corresponding submitted depth source");
}

void DisplayAndProducerReservation()
{
	SnapshotStore store(4);
	store.Start({5});
	FramePool producer(1);
	auto nativeSlot = producer.Reserve();
	const auto future = store.Reserve({}, nativeSlot);
	Require(store.ProducerReservation(future.id) == nativeSlot && store.Release(future.id),
			"Producer reservation was not retained");
	Require(producer.InUse() == 1, "Releasing a capture destroyed another reservation reader");
	nativeSlot.reset();
	Require(producer.InUse() == 0, "Unused producer capacity did not return");
	const auto empty = store.Reserve({CaptureKind::VirtualDisplay});
	const PresentationTick emptyTick{{0}, {Duration{100}}, PresentationKind::Empty};
	Require(store.PublishDisplay(empty.id, emptyTick) && store.Read(empty.id)->phase == SnapshotPhase::Empty,
			"Empty display refresh did not retain its actual result");
	const auto selected = store.Reserve({CaptureKind::VirtualDisplay});
	const auto frame = Frame({5}, 1);
	auto output = std::make_shared<Output>();
	output->status.store(CompletionStatus::Complete);
	PresentationTick tick{{2}, {Duration{120}}, PresentationKind::Repeat, frame.issued->key, output};
	tick.reprojection.source = std::make_shared<RetainedSource>();
	std::weak_ptr<const RotationalSource> retained = tick.reprojection.source;
	Require(store.PublishDisplay(selected.id, tick, frame.submitted), "Display snapshot publication failed");
	tick.reprojection.source.reset();
	Require(retained.expired() && !store.Read(selected.id)->display->reprojection.source,
			"Display screenshot retained per-layer inputs");
	store.Poll();
	Require(store.Read(selected.id)->phase == SnapshotPhase::Ready &&
				store.Read(selected.id)->display->tick == DisplayTick{2} &&
				store.Read(selected.id)->display->source == frame.issued->key,
			"Display source and tick identities were conflated");
	const auto unavailable = store.Reserve({CaptureKind::VirtualDisplay});
	Require(store.PublishDisplay(unavailable.id, tick) &&
				store.Read(unavailable.id)->phase == SnapshotPhase::Unavailable,
			"Missing source metadata was reported as successful inspection");
}

void ConcurrentObservation()
{
	SnapshotStore store(4);
	store.Start({6});
	std::atomic<bool> done{false};
	auto observer = std::async(std::launch::async,
							   [&]
							   {
								   while (!done.load())
								   {
									   store.Poll();
									   for (const auto id : store.Snapshot().captures)
										   store.Read(id);
								   }
							   });
	for (std::uint64_t index = 1; index <= 100; ++index)
	{
		CaptureReservation capture;
		do
		{
			capture = store.Reserve();
			if (capture.result == SnapshotAdmission::Capacity)
				std::this_thread::yield();
		} while (capture.result == SnapshotAdmission::Capacity);
		Require(capture.result == SnapshotAdmission::Accepted, "Concurrent capture reservation failed");
		const auto records = Frame({6}, index);
		Require(store.Bind(capture.id, records.issued, records.cancelled), "Concurrent capture bind failed");
		auto output = std::make_shared<Output>();
		const std::array outputs{CaptureOutput{{}, output}};
		store.Update(records.submitted, outputs);
		Require(store.Release(capture.id), "Concurrent capture release failed");
		output->status.store(CompletionStatus::Complete);
		store.Poll();
	}
	done.store(true);
	observer.get();
	store.Poll();
	Require(store.Snapshot().heldSlots == 0, "Concurrent capture retirement leaked capacity");
}

void DestructorReentry()
{
	struct LastReader final : FrameOutput
	{
		SnapshotStore* store = nullptr;
		CaptureId id;
		bool* observed = nullptr;
		CompletionStatus PollCompletion() const noexcept override
		{
			return CompletionStatus::Pending;
		}
		~LastReader() override
		{
			const auto reader = store->Read(id);
			const auto state = store->Snapshot();
			*observed = !reader && !state.running && state.captures.empty() && !state.retiring;
			store->Poll();
		}
	};
	bool observed = false;
	std::weak_ptr<LastReader> lifetime;
	{
		SnapshotStore store(1);
		store.Start({7});
		auto output = std::make_shared<LastReader>();
		output->store = &store;
		output->observed = &observed;
		lifetime = output;
		const auto frame = Frame({7}, 1);
		const auto capture = store.Pin(frame.submitted, {}, output);
		Require(capture.result == SnapshotAdmission::Accepted, "Pin destructor-reentry observer");
		output->id = capture.id;
		output.reset();
	}
	Require(observed && lifetime.expired(), "Store destruction exposed partially destroyed records to a last reader");
}
void TargetProducerOwnership()
{
	SnapshotStore store(8);
	store.Start({10});
	FramePool pool(2);
	const CaptureTarget raw{CaptureKind::LayerView, 0, 3};
	auto a = pool.Reserve(), b = pool.Reserve();
	const auto capture = store.Reserve(raw, a, b);
	const auto records = Frame({10}, 1);
	Require(store.Bind(capture.id, records.issued, records.cancelled), "Bind selected-view reservation");
	Require(store.RequestedTargets(records.issued->key) == std::vector<CaptureTarget>{raw} &&
				store.ProducerReservation(records.issued->key, raw) == a &&
				store.ProducerReservation(records.issued->key) == b,
			"Raw request lost its exact target or composed companion");
	Require(!store.SetProducer(records.issued->key, a, raw), "Producer replacement overwrote a live target");
	store.AbandonProducer(records.issued->key, raw);
	Require(!store.ProducerReservation(records.issued->key, raw) && store.ProducerReservation(records.issued->key) == b,
			"Abandoning raw preparation changed composed companion");
	Require(store.SetProducer(records.issued->key, a, raw), "Raw producer retry was not target-aware");
	store.AbandonProducer(records.issued->key);
	Require(store.ProducerReservation(records.issued->key, raw) == a && !store.ProducerReservation(records.issued->key),
			"Abandoning composed preparation changed raw producer");
	Require(store.SetProducer(records.issued->key, b), "Composed companion retry failed");
	Require(store.Release(capture.id) && store.RequestedTargets(records.issued->key).empty() &&
				!store.ProducerReservation(records.issued->key, raw) && !store.ProducerReservation(records.issued->key),
			"Released request remained in native capture inventory");
	a.reset();
	b.reset();
	Require(pool.InUse() == 0, "Released capture retained raw/composed producer budgets");
}

void EvictionAndPins()
{
	SnapshotStore store(3);
	store.Start({1});
	const auto pinned = store.Pin(Frame({1}, 1, 0).submitted);
	Require(store.SetPinned(pinned.id, true), "Pin snapshot");
	const auto displayed = store.Pin(Frame({1}, 2, 0).submitted);
	auto reader = store.Read(displayed.id);
	for (std::uint64_t frame = 3; frame < 203; ++frame)
	{
		if (store.Snapshot().heldSlots == store.Capacity())
			Require(store.EvictOldest(), "Evict oldest unpinned snapshot");
		Require(store.Pin(Frame({1}, frame, 0).submitted).result == SnapshotAdmission::Accepted,
			"200 snapshots exhausted retention");
		Require(store.Read(pinned.id)->pinned && store.Read(displayed.id).has_value(),
			"Evicted pinned or actively read snapshot");
	}
	Require(store.EvictOldest() && !store.EvictOldest(), "Eviction did not protect all remaining readers");
	reader.reset();
	Require(store.EvictOldest() && !store.Read(displayed.id), "Released reader still blocks eviction");
	Require(!store.EvictOldest() && store.SetPinned(pinned.id, false) && store.EvictOldest(),
		"Unpin did not restore capacity");
	const auto busy = store.Reserve();
	Require(!store.EvictOldest(), "Unsubmitted reservation was evicted");
	store.Release(busy.id);
	bool destroyed = false;
	auto output = std::make_shared<Output>();
	output->store = &store;
	output->destroyed = &destroyed;
	output->status = CompletionStatus::Complete;
	output->id = store.Pin(Frame({1}, 204).submitted, {}, output).id;
	store.Poll();
	Require(!store.EvictOldest(), "External image reader was evicted");
	output.reset();
	Require(store.EvictOldest() && destroyed, "Eviction did not release through reentrant retirement");
	FramePool pool(3);
	auto a = pool.Reserve(), b = pool.Reserve();
	pool.SetCapacity(1);
	Require(pool.InUse() == 2 && !pool.Reserve(), "Shrinking lost existing charges");
	a.reset();
	Require(!pool.Reserve(), "Full shrunken pool admitted another reader");
	b.reset();
	Require(bool(pool.Reserve()), "Retired charges did not restore shrunken capacity");
}

void SharedBudgetPolicy()
{
	SnapshotBudget::Configure(1024);
	Require(SnapshotBudget::Capacity() == 16, "Default budget does not reserve all four image copies");
	SnapshotStore store;
	FramePool pool{SnapshotBudget::Capacity};
	SnapshotBudget::Configure(256);
	Require(pool.Capacity() == 4 && store.Capacity() == pool.Capacity(), "Budget owners disagree");
	auto a = pool.Reserve(), b = pool.Reserve(), c = pool.Reserve();
	SnapshotBudget::Configure(128);
	Require(pool.InUse() == 3 && pool.Capacity() == 2 && !pool.Reserve(), "Budget shrink lost outstanding charges");
	a.reset();
	b.reset();
	Require(bool(pool.Reserve()), "Retirement did not make room after budget shrink");
	SnapshotBudget::Configure(1024);
	SnapshotBudget::ObserveImageBytes(32 * 1024 * 1024);
	Require(pool.Capacity() == 8 && store.Capacity() == 8, "Larger native allocations did not reduce every capacity");
	SnapshotBudget::ObserveImageBytes(1);
	Require(pool.Capacity() == 8, "Smaller images forgot outstanding larger allocations");
	SnapshotBudget::ObserveImageBytes(std::numeric_limits<std::uint64_t>::max());
	Require(pool.Capacity() == 0 && !pool.Reserve(), "Oversized image overflowed the budget calculation");
}

} // namespace

int main()
{
	EvictionAndPins();
	CaptureHistory();
	RetentionAndCompletion();
	PendingRetirementAndReentry();
	TargetsAndGenerations();
	TargetProducerOwnership();
	DepthRequiresSubmittedSource();
	DisplayAndProducerReservation();
	ConcurrentObservation();
	DestructorReentry();
	SharedBudgetPolicy();
	std::cout << "Snapshot ownership and reservation tests passed\n";
}
