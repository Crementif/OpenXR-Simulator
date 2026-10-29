#include "../TestSupport.h"
#include "presentation/FrameDiagnosticsJournal.h"
#include "presentation/FrameHistory.h"

#include <atomic>
#include <cstdlib>
#include <cstring>
#include <future>
#include <iostream>
#include <limits>
#include <new>
#include <stdexcept>
#include <thread>
#include <utility>

namespace
{
thread_local bool failDiagnosticAllocation = false;
}

[[gnu::noinline]] void* operator new(std::size_t size)
{
	if (std::exchange(failDiagnosticAllocation, false))
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

void MeasuredFrameTiming()
{
	using namespace simxr;
	FrameRecord frame;
	frame.key = {{3}, {8}};
	Require(!frame.CpuFrameTime(), "An unsubmitted frame invented CPU timing");
	frame.submission.emplace();
	auto diagnostics = std::make_shared<FrameDiagnostics>();
	frame.submission->diagnostics = diagnostics;
	FrameDiagnostic begin, end;
	begin.sequence = 1;
	begin.frame = frame.key;
	begin.hostTime = Duration{1000000};
	begin.kind = FrameTraceKind::WaitReturned;
	end = begin;
	end.sequence = 2;
	end.hostTime = Duration{6000000};
	end.kind = FrameTraceKind::Submitted;
	diagnostics->events = {begin, end};
	Require(frame.CpuFrameTime() == Duration{5000000}, "CPU span ignored host timestamps");
	diagnostics->events.front().frame.generation = {4};
	Require(!frame.CpuFrameTime(), "CPU span mixed generations");
	diagnostics->events = {end};
	Require(!frame.CpuFrameTime(), "Truncated history invented CPU timing");
	diagnostics->events = {begin, end};
	diagnostics->events.back().hostTime = Duration{0};
	Require(!frame.CpuFrameTime(), "CPU span accepted a backwards host clock");
	diagnostics->events.front().hostTime = Duration{-6000000};
	diagnostics->events.back().hostTime = Duration{-1000000};
	Require(frame.CpuFrameTime() == Duration{5000000}, "CPU span rejected an ordered negative host epoch");
	diagnostics->events.front().hostTime = Duration{-1000000};
	diagnostics->events.back().hostTime = Duration{4000000};
	Require(frame.CpuFrameTime() == Duration{5000000}, "CPU span rejected a representable interval across zero");
	diagnostics->events.front().hostTime = Duration{std::numeric_limits<Duration::rep>::min()};
	diagnostics->events.back().hostTime = Duration{-1};
	Require(frame.CpuFrameTime() == Duration{std::numeric_limits<Duration::rep>::max()},
			"CPU span rejected the largest representable interval");
	diagnostics->events.back().hostTime = Duration{0};
	Require(!frame.CpuFrameTime(), "CPU span overflowed an unrepresentable host interval");
}

void IndependentDiagnosticJournal()
{
	using namespace simxr;
	FrameDiagnosticsJournal* journalPointer = nullptr;
	bool clockFailure = false;
	std::int64_t hostTime = 100;
	FrameDiagnosticsJournal journal(3, [&] {
		if (clockFailure)
			throw std::runtime_error("Clock unavailable");
		Require(journalPointer->Read(0, 0).capacity == 3, "Clock callback ran under the journal mutex");
		return Duration{hostTime++};
	});
	journalPointer = &journal;
	const FrameKey first{{1}, {7}}, restarted{{2}, {7}};
	failDiagnosticAllocation = true;
	const auto initial = journal.Trace(FrameTraceKind::Issued, first, {Duration{10}});
	const bool allocationFree = failDiagnosticAllocation;
	failDiagnosticAllocation = false;
	Require(allocationFree && initial.sequence == 1 && initial.hostTime == Duration{100},
			"Preallocated observation allocated or ignored the injected host clock");
	for (std::uint64_t sequence = 2; sequence <= 5; ++sequence)
		journal.Trace(FrameTraceKind::DisplayRepeated, sequence % 2 ? first : restarted,
					  {Duration{static_cast<std::int64_t>(sequence)}}, XR_SUCCESS, FrameDiagnosticCategory::Performance,
					  sequence);
	const auto page = journal.Read(0, 1, first);
	Require(page.capacity == 3 && page.oldest == 3 && page.next == 6 && page.dropped == 2 && page.truncated &&
				page.events.size() == 1 && page.events.front().sequence == 3,
			"Filtered pages lost global drop/cursor information or confused restarted frame identities");
	const auto continuation = journal.Read(page.events.back().sequence, 1, first);
	Require(continuation.events.size() == 1 && continuation.events.front().sequence == 5 && !continuation.truncated &&
				journal.Read(3, 0, first).truncated &&
				journal.Read(std::numeric_limits<std::uint64_t>::max()).events.empty(),
			"Journal continuation, zero-length query or exhausted cursor semantics changed");
	clockFailure = true;
	const auto failed = journal.Trace(FrameTraceKind::GpuComplete, first, {Duration{20}});
	Require(failed.sequence == 0 && failed.hostTime == Duration{} && journal.Read().next == 6,
			"Clock failure must not throw, consume a sequence or invent a recorded completion");
	clockFailure = false;
	const std::string unicode = std::string(191, 'x') + "\xe2\x82\xac";
	const auto truncated = journal.Trace(FrameTraceKind::Validation, first, {Duration{21}}, XR_ERROR_VALIDATION_FAILURE,
										 FrameDiagnosticCategory::Validation, 17, unicode);
	Require(truncated.detailTruncated && truncated.Detail() == std::string(191, 'x') &&
				page.events.front().sequence == 3 && page.events.front().related == 3,
			"Truncation split UTF-8 or overwriting the ring mutated a retained page");
	for (std::size_t capacity : {std::size_t{0}, std::size_t{8193}})
	{
		bool rejected = false;
		try
		{
			FrameDiagnosticsJournal invalid(capacity);
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Journal accepted invalid bounded capacity");
	}
}

void ConcurrentDiagnosticJournal()
{
	using namespace simxr;
	std::atomic<std::int64_t> hostTime{1};
	FrameDiagnosticsJournal journal(17, [&] { return Duration{hostTime.fetch_add(1)}; });
	std::array<std::thread, 4> writers;
	for (std::size_t index = 0; index < writers.size(); ++index)
	{
		writers[index] = std::thread([&, index] {
			for (std::uint64_t event = 0; event < 100; ++event)
				journal.Trace(FrameTraceKind::WaitReturned, {{1}, {index + 1}}, {Duration{1}});
		});
	}
	for (auto& writer : writers)
		writer.join();
	const auto snapshot = journal.Read();
	Require(snapshot.events.size() == 17 && snapshot.next == 401 && snapshot.oldest == 384 && snapshot.dropped == 383,
			"Concurrent observation lost sequences or exceeded ring capacity");
	for (std::size_t index = 0; index < snapshot.events.size(); ++index)
		Require(snapshot.events[index].sequence == snapshot.oldest + index &&
					snapshot.events[index].hostTime.count() > 0,
				"Concurrent journal published a partial or unordered slot");
}

void DiagnosticClockLifetime()
{
	using namespace simxr;
	FrameDiagnosticsJournal* journalPointer = nullptr;
	bool observed = false;

	struct ClockOwner
	{
		FrameDiagnosticsJournal*& journal;
		bool& observed;

		ClockOwner(FrameDiagnosticsJournal*& owner, bool& released) : journal(owner), observed(released) {}

		~ClockOwner()
		{
			observed = journal->Read().events.size() == 1;
		}
	};

	auto clockOwner = std::make_shared<ClockOwner>(journalPointer, observed);
	auto journal = std::make_unique<FrameDiagnosticsJournal>(2, [clockOwner] { return Duration{42}; });
	clockOwner.reset();
	journalPointer = journal.get();
	journal->Trace(FrameTraceKind::Issued, {{1}, {1}}, {Duration{1}});
	journal.reset();
	Require(observed, "Clock callback captures outlived the journal mutex/storage during last-reader release");
}

void RetainedActionInterpretation()
{
	using namespace simxr;
	FrameHistory history(2);
	const auto generation = history.StartGeneration();
	std::shared_ptr<const FrameRecord> reader;
	std::weak_ptr<const ActionInspectionDictionary> dictionaryLifetime;
	XrActionSet set;
	XrAction action;
	XrPath user, source, profile;
	{
		EnabledExtensions enabled;
		ActionRegistry registry(reinterpret_cast<XrInstance>(400), enabled);
		XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
		std::strcpy(setInfo.actionSetName, "frame-actions");
		std::strcpy(setInfo.localizedActionSetName, "Frame Actions");
		setInfo.priority = 17;
		Require(registry.CreateSet(setInfo, set) == XR_SUCCESS, "Create historical frame set");
		Require(registry.StringToPath("/user/hand/left", user) == XR_SUCCESS &&
					registry.StringToPath("/user/hand/left/input/trigger/value", source) == XR_SUCCESS &&
					registry.StringToPath("/interaction_profiles/oculus/touch_controller", profile) == XR_SUCCESS,
				"Create historical frame paths");
		XrActionCreateInfo create{XR_TYPE_ACTION_CREATE_INFO};
		std::strcpy(create.actionName, "frame-trigger");
		std::strcpy(create.localizedActionName, "Frame Trigger");
		create.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
		create.countSubactionPaths = 1;
		create.subactionPaths = &user;
		Require(registry.CreateAction(set, create, action) == XR_SUCCESS, "Create historical frame action");
		const XrActionSuggestedBinding binding{action, source};
		const XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														   profile, 1, &binding};
		Require(registry.SuggestBindings(suggest) == XR_SUCCESS, "Bind historical frame action");
		ActionSyncState sync(registry);
		const XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
		Require(sync.Attach(attach) == XR_SUCCESS, "Attach historical frame actions");
		const XrActiveActionSet active{set, user};
		const XrActionsSyncInfo synchronize{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
		InputSnapshot input;
		input.time = RuntimeTime{Duration{500}};
		input.controllers[0].trigger = 0.75f;
		Require(sync.Sync(synchronize, input, true) == XR_SUCCESS, "Sync historical frame action");
		const FramePrediction prediction{{1}, {1}, {Duration{501}}, Duration{10}};
		history.Issue(prediction);
		history.Begin(prediction.id);
		FrameSubmissionState submitted;
		submitted.displayTime = prediction.displayTime;
		submitted.actions = sync.Snapshot();
		dictionaryLifetime = submitted.actions.dictionary;
		Require(history.Submit(prediction.id, submitted) == XR_SUCCESS, "Record historical action state");
		reader = history.Find({generation, prediction.id});
		input.controllers[0].trigger = 0.1f;
		input.time = RuntimeTime{Duration{600}};
		Require(sync.Sync(synchronize, input, false) == XR_SESSION_NOT_FOCUSED &&
					registry.DestroyAction(action) == XR_SUCCESS && registry.DestroySet(set) == XR_SUCCESS,
				"Retire current frame action resources");
		sync.Reset();
	}
	for (std::uint64_t index = 2; index < 5; ++index)
	{
		history.StartGeneration();
		const FramePrediction prediction{
			{index}, {index}, {Duration{600 + static_cast<std::int64_t>(index)}}, Duration{10}};
		history.Issue(prediction);
		history.Begin(prediction.id);
		FrameSubmissionState clear;
		clear.displayTime = prediction.displayTime;
		history.Submit(prediction.id, std::move(clear));
	}
	Require(!history.Find({generation, {1}}) && reader && reader->submission &&
				reader->submission->actions.serial == 1 && reader->submission->actions.time.value.count() == 500 &&
				reader->submission->actions.focused &&
				reader->submission->actions.actions.front().values.at(user).scalar == 0.75f,
			"Retained frame lost exact synchronized actions after history eviction and registry teardown");
	const auto& dictionary = *reader->submission->actions.dictionary;
	Require(dictionary.FindAction(action)->name == "frame-trigger" && dictionary.FindAction(action)->live &&
				dictionary.FindAction(action)->localizedName == "Frame Trigger" &&
				dictionary.FindSet(set)->name == "frame-actions" &&
				dictionary.FindSet(set)->localizedName == "Frame Actions" && dictionary.FindSet(set)->priority == 17 &&
				dictionary.FindSet(set)->live && dictionary.PathName(source) == "/user/hand/left/input/trigger/value" &&
				dictionary.PathName(profile) == "/interaction_profiles/oculus/touch_controller",
			"Frame action interpretation depended on mutable/destroyed registry names");
	reader.reset();
	Require(dictionaryLifetime.expired(), "Retained dictionary leaked after its last historical frame reader");
}

void SubmissionObserverLifetime()
{
	using namespace simxr;
	std::shared_ptr<FrameDiagnosticsJournal> observer;
	FrameKey key;
	{
		FrameHistory history(2, 16);
		observer = history.DiagnosticsJournal();
		key = {history.StartGeneration(), {1}};
		const FramePrediction prediction{key.frame, {1}, {Duration{100}}, Duration{10}};
		history.Issue(prediction);
		history.Begin(prediction.id);
		FrameSubmissionState submission;
		submission.displayTime = prediction.displayTime;
		const auto result = history.Submit(prediction.id, submission, [&] {
			observer->Trace(FrameTraceKind::NativeSubmitted, key, prediction.displayTime);
			failDiagnosticAllocation = true;
			return XR_SUCCESS;
		});
		const bool allocationFree = std::exchange(failDiagnosticAllocation, false);
		const auto record = history.Find(key);
		const auto& events = record->submission->diagnostics->events;
		Require(result == XR_SUCCESS && allocationFree && events.size() == 4 &&
					events[2].kind == FrameTraceKind::NativeSubmitted && events[3].kind == FrameTraceKind::Submitted,
				"Accepted native milestone must enter immutable CPU trace without post-acceptance allocation");
		FrameDiagnostics unreserved;
		Require(!observer->ReadInto(unreserved, 0, 16, key) && unreserved.events.empty(),
				"Allocation-free read must reject insufficient caller storage");
	}
	Require(observer->Trace(FrameTraceKind::ImageRetired, key, {Duration{100}}).sequence == 5 &&
				observer->Read(0, 16, key).events.back().kind == FrameTraceKind::ImageRetired,
			"Late native image retirement must not depend on a destroyed session/history");
	FrameHistory crowded(2, 1024);
	const auto crowdedObserver = crowded.DiagnosticsJournal();
	const FrameKey crowdedKey{crowded.StartGeneration(), {1}};
	const FramePrediction prediction{crowdedKey.frame, {1}, {Duration{100}}, Duration{10}};
	crowded.Issue(prediction);
	crowded.Begin(prediction.id);
	FrameSubmissionState submission;
	submission.displayTime = prediction.displayTime;
	const auto result = crowded.Submit(prediction.id, std::move(submission), [&] {
		for (std::size_t index = 0; index < 300; ++index)
			crowdedObserver->Trace(FrameTraceKind::Validation, crowdedKey, prediction.displayTime);
		failDiagnosticAllocation = true;
		return XR_SUCCESS;
	});
	const bool allocationFree = std::exchange(failDiagnosticAllocation, false);
	const auto record = crowded.Find(crowdedKey);
	const auto& diagnostics = *record->submission->diagnostics;
	Require(result == XR_SUCCESS && allocationFree && diagnostics.truncated && diagnostics.events.size() == 256 &&
				diagnostics.events.back().kind == FrameTraceKind::Submitted &&
				diagnostics.events.back().sequence == diagnostics.next - 1,
			"Bounded CPU trace lost the accepted submission milestone or allocated after native acceptance");
}

void DiagnosticHistory()
{
	using namespace simxr;
	FrameHistory history(2, 4);
	const auto generation = history.StartGeneration();
	const FramePrediction prediction{{1}, {1}, {Duration{100}}, Duration{10}};
	history.Issue(prediction);
	history.Begin(prediction.id);
	FrameSubmissionState submission;
	submission.displayTime = prediction.displayTime;
	Require(history.Submit(prediction.id, submission, [] { return XR_ERROR_OUT_OF_MEMORY; }) == XR_ERROR_OUT_OF_MEMORY,
			"Rejected native work retains begun frame");
	auto diagnostics = history.Diagnostics();
	Require(diagnostics.events.size() == 3 && diagnostics.events.back().kind == FrameTraceKind::ApiError &&
				diagnostics.events.back().category == FrameDiagnosticCategory::ApiError &&
				diagnostics.events.back().result == XR_ERROR_OUT_OF_MEMORY,
			"Native rejection is explicit API error, never completion");
	Require(history.Submit(prediction.id, submission) == XR_SUCCESS, "Submit trace retry");
	const auto retained = history.Find({generation, prediction.id});
	Require(retained->submission->diagnostics &&
				retained->submission->diagnostics->events.back().kind == FrameTraceKind::Submitted,
			"Immutable submission retains actual accepted CPU milestone");
	const auto original = retained->submission->diagnostics->events.size();
	for (std::uint64_t tick = 1; tick <= 8; ++tick)
		history.Trace(FrameTraceKind::DisplayRepeated, {generation, prediction.id},
					  {Duration{static_cast<std::int64_t>(tick + 100)}}, XR_SUCCESS,
					  FrameDiagnosticCategory::Performance, tick);
	diagnostics = history.Diagnostics(0, 2);
	Require(diagnostics.capacity == 4 && diagnostics.dropped == 8 && diagnostics.events.size() == 2 &&
				diagnostics.truncated,
			"Diagnostic ring has bounded explicit overflow and page continuation");
	Require(diagnostics.events[0].sequence == 9 && diagnostics.events[1].sequence == 10 &&
				diagnostics.events[1].related == 6,
			"Trace sequence and refresh identity preserved");
	const auto next = history.Diagnostics(diagnostics.events.back().sequence, 2, FrameKey{generation, prediction.id});
	Require(next.events.size() == 2 && !next.truncated && next.events.back().related == 8 &&
				next.events.front().hostTime.count() > 0,
			"Trace paging keeps monotonic host observation timestamp");
	Require(retained->submission->diagnostics->events.size() == original,
			"Ring overwrite cannot mutate retained CPU trace");
	const std::string message(300, 'x');
	const auto warning = history.Trace(FrameTraceKind::Validation, {generation, prediction.id}, prediction.displayTime,
									   XR_ERROR_VALIDATION_FAILURE, FrameDiagnosticCategory::Validation, 17, message);
	Require(warning.detailTruncated && warning.Detail().size() == 192 && warning.related == 17 &&
				warning.category == FrameDiagnosticCategory::Validation,
			"Validation payload is owned and explicitly bounded independently from timing");
	bool failed = false;
	try
	{
		history.Diagnostics(0, 257);
	}
	catch (const std::invalid_argument&)
	{
		failed = true;
	}
	Require(failed, "Diagnostic page is bounded");
	FrameHistory cancelled;
	const auto cancelledGeneration = cancelled.StartGeneration();
	cancelled.Issue(prediction);
	cancelled.CancelPending();
	const auto cancelledEvents = cancelled.Diagnostics();
	Require(cancelledEvents.events.back().kind == FrameTraceKind::Cancelled &&
				cancelledEvents.events.back().frame.generation == cancelledGeneration,
			"Shutdown records actual CPU cancellation without false GPU completion");
}

int main()
{
	using namespace simxr;
	FrameHistory history(2);
	const auto generation = history.StartGeneration();
	const FramePrediction first{{1}, {1}, {Duration{101}}, Duration{10}};
	const FramePrediction second{{2}, {2}, {Duration{111}}, Duration{10}};
	history.Issue(first);
	const auto issued = history.Find({generation, {1}});
	history.Begin({1});
	history.Issue(second);
	history.Begin({2}, FrameId{1});
	Require(issued->stage == FrameStage::Issued && history.Find({generation, {1}})->stage == FrameStage::Discarded,
			"History mutation changed retained reader");
	FrameSubmissionState submission;
	submission.displayTime = {Duration{123}};
	submission.layerCount = 0;
	submission.input.devices.push_back(
		{{InputDeviceKind::Tracker, 1}, {"/user/vive_tracker_htcx/role/waist"}, "profile"});
	submission.tracking = TrackingSnapshot{};
	submission.tracking->head.pose.position.x = 3;
	history.Submit({2}, submission);
	const auto retained = history.Find({generation, {2}});
	submission.input.devices[0].userPaths[0] = "changed";
	submission.tracking->head.pose.position.x = 7;
	Require(retained->stage == FrameStage::Submitted && retained->submission && retained->submission->layerCount == 0 &&
				retained->submission->displayTime.value.count() == 123 &&
				retained->prediction.displayTime.value.count() == 111 &&
				retained->submission->tracking->head.pose.position.x == 3 &&
				retained->submission->input.devices[0].userPaths[0] != "changed",
			"Submission metadata was borrowed or display time confused with prediction");
	const auto restarted = history.StartGeneration();
	Require(restarted != generation, "Session restart reused history generation");
	history.Issue(first);
	Require(!history.Find({generation, {1}}) && history.Snapshot().evicted == 1,
			"Bounded history did not report eviction");
	history.CancelPending();
	Require(history.Find({restarted, {1}})->stage == FrameStage::Cancelled, "Teardown fabricated completed submission");
	history.StartGeneration();
	history.Issue(first);
	history.Begin({1});
	history.Issue(second);
	bool rejected = false;
	try
	{
		history.Issue({{3}, {3}, {Duration{121}}, Duration{10}});
	}
	catch (const std::length_error&)
	{
		rejected = true;
	}
	Require(rejected && history.Snapshot().frames.size() == 2, "Pending boundary frames evicted");
	std::atomic<bool> stop = false;
	auto reader = std::async(std::launch::async, [&] {
		while (!stop.load())
			for (const auto& frame : history.Snapshot().frames)
				Require(frame->prediction.id.value != 0, "Partial history record published");
	});
	for (int count = 0; count < 100; ++count)
	{
		history.StartGeneration();
		history.Issue(first);
		history.Begin({1});
		history.Submit({1}, submission);
	}
	stop.store(true);
	reader.get();
	Require(retained->submission->tracking->head.pose.position.x == 3 && issued->stage == FrameStage::Issued,
			"Eviction invalidated retained CPU snapshots");
	FrameHistory other;
	Require(other.StartGeneration() != history.Snapshot().generation, "Different sessions share a generation");
	const auto nativeGeneration = other.StartGeneration();
	other.Issue(first);
	other.Begin(first.id);
	bool called = false;
	Require(other.Submit(first.id, submission,
						 [&] {
							 called = true;
							 return XR_ERROR_RUNTIME_FAILURE;
						 }) == XR_ERROR_RUNTIME_FAILURE &&
				called && other.Find({nativeGeneration, first.id})->stage == FrameStage::Begun,
			"Failed native admission committed frame history");
	Require(other.Submit(first.id, submission, [] { return XR_SUCCESS; }) == XR_SUCCESS &&
				other.Find({nativeGeneration, first.id})->stage == FrameStage::Submitted,
			"Native admission retry lost submission identity");
	FrameHistory reserved(2);
	const auto reservedGeneration = reserved.StartGeneration();
	reserved.Issue(first);
	reserved.Begin(first.id);
	reserved.Submit(first.id, submission);
	reserved.Issue(second);
	reserved.Begin(second.id);
	reserved.Submit(second.id, submission);
	bool reservationFailed = false;
	try
	{
		reserved.Issue({{3}, {3}, {Duration{121}}, Duration{10}}, [](auto, auto) { throw std::bad_alloc{}; });
	}
	catch (const std::bad_alloc&)
	{
		reservationFailed = true;
	}
	Require(reservationFailed && reserved.Find({reservedGeneration, first.id}) &&
				!reserved.Find({reservedGeneration, {3}}) && reserved.Snapshot().evicted == 0,
			"Failed capture binding committed history insertion or eviction");
	std::shared_ptr<const FrameRecord> cancellation;
	reserved.Issue({{3}, {3}, {Duration{121}}, Duration{10}}, [&](auto issued, auto cancelled) {
		Require(issued->key == FrameKey{reservedGeneration, {3}} && cancelled->stage == FrameStage::Cancelled,
				"Reservation did not receive prepared immutable frame records");
		cancellation = std::move(cancelled);
	});
	reserved.CancelPending();
	Require(reserved.Find({reservedGeneration, {3}}) == cancellation && reserved.Snapshot().evicted == 1,
			"Successful reservation lost preallocated cancellation ownership");
	MeasuredFrameTiming();
	IndependentDiagnosticJournal();
	ConcurrentDiagnosticJournal();
	DiagnosticClockLifetime();
	RetainedActionInterpretation();
	DiagnosticHistory();
	SubmissionObserverLifetime();
	std::cout << "Frame history tests passed\n";
}
