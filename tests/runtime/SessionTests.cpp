#include "../TestSupport.h"
#include "graphics/SnapshotBudget.h"
#include "runtime/Instance.h"
#include "runtime/Session.h"

#include <algorithm>
#include <array>
#include <atomic>
#include <chrono>
#include <cmath>
#include <cstring>
#include <functional>
#include <future>
#include <iostream>
#include <latch>
#include <memory>
#include <stdexcept>

namespace
{
void TestPresentationPolicyLifecycle()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(902), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(903), instance);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(timing.epoch) == XR_SUCCESS, "Ready presentation policy session");
	Require(session.Begin(begin, timing, static_cast<PresentationPolicy>(99)) == XR_ERROR_VALIDATION_FAILURE &&
				!session.Snapshot().running,
			"Invalid presentation policy started the session");
	Require(session.Begin(begin, timing, PresentationPolicy::Rotational) == XR_SUCCESS &&
				session.Display().Policy() == PresentationPolicy::Rotational,
			"Session did not select rotational display");
	Require(session.Begin(begin, timing) == XR_ERROR_SESSION_RUNNING &&
				session.Display().Policy() == PresentationPolicy::Rotational,
			"Repeated begin changed active display policy");
	Require(session.RequestExit(timing.epoch) == XR_SUCCESS && session.End(timing.epoch) == XR_SUCCESS,
			"End rotational display session");
	Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS &&
				session.Display().Policy() == PresentationPolicy::Repeat,
			"Restart did not restore default repeat policy");
	session.Close();
}

void TestConcurrentNativeControlSynchronization()
{
	using namespace simxr;
	using namespace std::chrono_literals;
	Instance instance(reinterpret_cast<XrInstance>(900), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(901), instance);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
			"Begin native-control synchronization session");

	auto& registry = instance.Actions();
	const auto path = [&](const char* name)
	{
		XrPath result = XR_NULL_PATH;
		Require(registry.StringToPath(name, result) == XR_SUCCESS, "Create native-control action path");
		return result;
	};
	XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(setInfo.actionSetName, "native_controls");
	std::strcpy(setInfo.localizedActionSetName, "Native controls");
	XrActionSet actionSet = XR_NULL_HANDLE;
	Require(registry.CreateSet(setInfo, actionSet) == XR_SUCCESS, "Create native-control action set");
	const std::array userPaths{path("/user/hand/left"), path("/user/hand/right")};
	XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
	std::strcpy(actionInfo.actionName, "trigger");
	std::strcpy(actionInfo.localizedActionName, "Trigger");
	actionInfo.actionType = XR_ACTION_TYPE_FLOAT_INPUT;
	actionInfo.countSubactionPaths = static_cast<std::uint32_t>(userPaths.size());
	actionInfo.subactionPaths = userPaths.data();
	XrAction trigger = XR_NULL_HANDLE;
	Require(registry.CreateAction(actionSet, actionInfo, trigger) == XR_SUCCESS,
			"Create native-control trigger action");
	const std::array bindings{XrActionSuggestedBinding{trigger, path("/user/hand/left/input/trigger/value")},
							  XrActionSuggestedBinding{trigger, path("/user/hand/right/input/trigger/value")}};
	const XrInteractionProfileSuggestedBinding suggested{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
														 path("/interaction_profiles/oculus/touch_controller"),
														 static_cast<std::uint32_t>(bindings.size()), bindings.data()};
	Require(registry.SuggestBindings(suggested) == XR_SUCCESS, "Bind native-control trigger action");
	const XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &actionSet};
	Require(session.Attach(attach) == XR_SUCCESS, "Attach native-control action set");
	const XrActiveActionSet active{actionSet, XR_NULL_PATH};
	const XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};

	ControllerControls controlled;
	controlled.trigger = 0.875f;
	controlled.primary = true;
	const CommandSource source{CommandSourceKind::Manual, 902};
	const CommandTransaction overrideControls{
		1,
		source,
		{FrameId{1}, {}},
		CommandLatePolicy::Reject,
		{TakeControl{ControlChannel::LeftControls}, SetControllerControls{0, controlled}}};
	Require(session.Commands().Submit(overrideControls).result == CommandResult::Queued,
			"Queue controller override before native input");
	const auto staleInput = session.Input().Snapshot();
	const auto publishFrame = [&]
	{
		FramePrediction prediction;
		Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
					session.EndFrame(prediction) == XR_SUCCESS,
				"Publish concurrent deterministic frame");
	};
	publishFrame();
	Require(session.PublishInput(staleInput) == XR_ERROR_TIME_INVALID,
			"Standalone session publication must still reject an obsolete full input snapshot");

	std::array<ControllerControls, 2> nativeControls;
	nativeControls[0].trigger = 0.125f;
	Require(session.SyncActions(sync, nativeControls) == XR_SUCCESS,
			"Atomic native controls must use current session time after a frame advances");
	std::latch nativeSyncStarted{1};
	auto frames = std::async(std::launch::async,
							 [&]
							 {
								 nativeSyncStarted.wait();
								 for (unsigned frame = 0; frame < 256; ++frame)
								 {
									 publishFrame();
									 std::this_thread::yield();
								 }
							 });
	auto previousInputTime = session.Input().Snapshot().time;
	auto previousActions = session.Actions().Snapshot();
	unsigned syncCount = 0;
	do
	{
		nativeControls[1].trigger = syncCount % 2 ? 0.25f : 0.75f;
		const auto result = session.SyncActions(sync, nativeControls);
		// Always release the finite frame worker before any assertion can throw.
		if (syncCount == 0)
			nativeSyncStarted.count_down();
		Require(result == XR_SUCCESS, "Native controls raced frame time during xrSyncActions");
		const auto input = session.Input().Snapshot();
		const auto actions = session.Actions().Snapshot();
		Require(input.time >= previousInputTime && actions.time >= previousActions.time && actions.time <= input.time &&
					actions.serial > previousActions.serial,
				"Concurrent native synchronization regressed input/action timestamps or synchronization identity");
		Require(input.controllers[0].trigger == controlled.trigger && input.controllers[0].primary,
				"Native synchronization replaced an owned controller override");
		for (std::size_t hand = 0; hand < userPaths.size(); ++hand)
		{
			const XrActionStateGetInfo query{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, trigger, userPaths[hand]};
			XrActionStateFloat state{XR_TYPE_ACTION_STATE_FLOAT};
			const float expected = hand == 0 ? controlled.trigger : nativeControls[1].trigger;
			Require(session.Actions().GetFloat(query, state) == XR_SUCCESS && state.isActive &&
						state.currentState == expected,
					"Synchronized trigger action lost native input or its override");
		}
		previousInputTime = input.time;
		previousActions = actions;
		++syncCount;
		std::this_thread::yield();
	} while (syncCount < 256 || frames.wait_for(0ms) != std::future_status::ready);
	frames.get();
	Require(session.Snapshot().lastSubmitted == FrameId{257}, "Concurrent publisher did not finish every frame");

	InputState standalone;
	InputSnapshot newer;
	newer.time = {Duration{20}};
	Require(standalone.Publish(newer) == XR_SUCCESS, "Prepare standalone timestamp validation");
	const auto accepted = standalone.Snapshot();
	newer.time = {Duration{19}};
	Require(standalone.Publish(newer) == XR_ERROR_TIME_INVALID && standalone.Snapshot().time == accepted.time &&
				standalone.Revision() == accepted.revision,
			"Native synchronization weakened standalone input validation");
}

void TestReentrantCaptureCollection()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(78), {}, XR_MAKE_VERSION(1, 0, 27));
	auto owner = std::make_unique<Session>(reinterpret_cast<XrSession>(79), instance);
	auto& session = *owner;
	unsigned collections = 0;
	FramePool pool(2,
				   [&]
				   {
					   Require(session.Snapshot().running, "Collection callback observed a stopped session");
					   ++collections;
				   });
	Require(session.ConfigureCapturePool(pool) == XR_SUCCESS, "Configure reentrant collection pool");
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(config.epoch) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Begin reentrant collection session");
	session.Scheduler().Pause();
	Require(session.Display().Pause(), "Pause reentrant collection display");
	const auto capture = session.ReserveCapture();
	Require(capture.result == SnapshotAdmission::Accepted && collections == 1 && pool.InUse() == 1,
			"Reservation collection ran under session lock or ran again during protected commit");
	Require(session.CancelCaptureReservations() == 1 && pool.InUse() == 0, "Cancel unused collection reservation");
	FramePrediction prediction;
	Require(session.Scheduler().GrantPermits() == XR_SUCCESS && session.WaitFrame(prediction) == XR_SUCCESS &&
				session.BeginFrame() == XR_SUCCESS,
			"Issue unreserved boundary frame");
	const auto boundary = session.CaptureBoundary();
	Require(boundary.result == SnapshotAdmission::Accepted && boundary.captures.size() == 1 && collections == 2 &&
				pool.InUse() == 1 &&
				session.Captures().Read(boundary.captures.front())->frame ==
					FrameKey{session.Snapshot().generation, prediction.id},
			"Boundary collection ran under session lock or failed to pin the exact frame");
	session.Close();
	Require(pool.InUse() == 0, "Collection boundary shutdown retained unused producer capacity");
}

void TestCaptureProducerRetry()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(80), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(81), instance);
	FramePool pool(1);
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.ConfigureCapturePool(pool) == XR_SUCCESS && session.Ready(config.epoch) == XR_SUCCESS &&
				session.Begin(begin, config) == XR_SUCCESS,
			"Begin capture preparation retry");
	session.Scheduler().Pause();
	Require(session.Display().Pause(), "Pause capture preparation retry");
	const auto capture = session.ReserveCapture();
	FramePrediction prediction;
	Require(capture.result == SnapshotAdmission::Accepted && session.Scheduler().GrantPermits() == XR_SUCCESS &&
				session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
			"Issue capture preparation retry");
	const FrameKey key{session.Snapshot().generation, prediction.id};
	std::shared_ptr<FrameReservation> first;
	Require(session.PrepareCaptureProducer(key, first) == XR_SUCCESS && first && pool.Consume(first),
			"Native preparation did not reuse its pre-admission reservation");
	session.Captures().AbandonProducer(key);
	std::shared_ptr<FrameReservation> replacement;
	Require(session.PrepareCaptureProducer(key, replacement) == XR_ERROR_LIMIT_REACHED && !replacement &&
				session.Captures().Read(capture.id)->phase == SnapshotPhase::Begun,
			"Abandoned preparation fabricated capacity while a native reader remained");
	first.reset();
	Require(session.PrepareCaptureProducer(key, replacement) == XR_SUCCESS && replacement && !replacement->Consumed() &&
				pool.InUse() == 1 && session.Captures().Read(capture.id)->frame == key &&
				session.Scheduler().Snapshot().permits == 0,
			"Failed preparation could not reserve a new producer for the same capture without another admission");
	replacement.reset();
	Require(session.EndFrame(prediction) == XR_SUCCESS &&
				session.Captures().Read(capture.id)->phase == SnapshotPhase::Clear && pool.InUse() == 0,
			"Retried capture lost its identity or retained an unused clear producer");
}

class CaptureOutput final : public simxr::FrameOutput
{
  public:
	explicit CaptureOutput(std::shared_ptr<simxr::FrameReservation> reservation) : slot(std::move(reservation)) {}
	~CaptureOutput() override
	{
		if (destroy)
			destroy();
	}
	simxr::CompletionStatus PollCompletion() const noexcept override
	{
		return status.load();
	}
	std::shared_ptr<simxr::FrameReservation> slot;
	std::function<void()> destroy;
	std::atomic<simxr::CompletionStatus> status{simxr::CompletionStatus::Pending};
};

void TestSequenceCaptureAdmission()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(982), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(983), instance);
	FramePool pool(1);
	FrameSchedulerConfig config;
	config.period = Duration{1000000};
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.ConfigureCapturePool(pool) == XR_SUCCESS && session.Ready(config.epoch) == XR_SUCCESS &&
		session.Begin(begin, config) == XR_SUCCESS, "Begin real-time sequence capture session");
	const CommandSource owner{CommandSourceKind::Mcp, 47};
	PoseSequenceStep step;
	step.capture = true;
	step.head = HeadPosePatch{};
	step.head->position[0] = 4;
	const auto sequence = session.Commands().StartPoseSequence(owner,
		std::make_shared<const PoseSequence>(std::vector{step}, true));
	FramePrediction frame;
	Require(session.WaitFrame(frame) == XR_SUCCESS, "Sequence capture required a manual frame permit");
	const auto generation = session.Snapshot().generation;
	const FrameKey first{generation, frame.id};
	const auto issued = session.History().Find(first);
	const auto receipt = issued->sequenceCapture;
	Require(receipt && receipt->request.sequence == sequence->id && receipt->request.owner == owner &&
		receipt->reservation.result == SnapshotAdmission::Accepted && receipt->request.frame == frame.id &&
		!session.Scheduler().Snapshot().paused && !session.Scheduler().Snapshot().permits &&
		session.Commands().At(frame.id)->configuration.tracking.head.pose.position.x == 4,
		"Sequence capture changed pacing or lost its exact pose/frame identity");
	Require(session.BeginFrame() == XR_SUCCESS, "Begin sequence capture");
	auto token = session.Captures().ProducerReservation(first);
	Require(token && pool.Consume(token), "Sequence capture has no admitted native producer");
	auto output = std::make_shared<CaptureOutput>(std::move(token));
	const auto layers = std::make_shared<const FrameLayers>(frame.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
		std::vector<LayerDescription>{QuadLayerDescription{}});
	const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO, nullptr, frame.displayTime.value.count(),
		XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1, nullptr};
	Require(session.EndFrame(frame, &end, layers, output) == XR_SUCCESS &&
		session.Captures().Read(receipt->reservation.id)->phase == SnapshotPhase::GpuPending,
		"Sequence capture bypassed producer completion");
	Require(session.WaitFrame(frame) == XR_SUCCESS, "Capture pressure stopped real-time frame admission");
	const auto pressure = session.History().Find({generation, frame.id})->sequenceCapture;
	Require(pressure && pressure->reservation.result == SnapshotAdmission::Capacity &&
		!pressure->reservation.id.value && pressure->request.frame == frame.id && frame.id != first.frame &&
		session.BeginFrame() == XR_SUCCESS && session.EndFrame(frame) == XR_SUCCESS,
		"Capture pressure hid failure or substituted a later frame");
	auto reader = session.Captures().Read(receipt->reservation.id);
	Require(reader && reader->phase == SnapshotPhase::GpuPending && reader->metadata->key == first &&
		reader->metadata->sequenceCapture->request.sequence == sequence->id,
		"Pending sequence image lost its original receipt");
	Require(session.WithCommandGeneration(generation,
		[&]
		{
			session.ReleaseCommandSource(owner, true);
		}) &&
		!session.Captures().Read(receipt->reservation.id) && !session.Commands().PoseSequenceStatusFor(owner)->active,
		"Disconnect retained sequence admission or an owned capture ticket");
	session.Captures().Poll();
	Require(session.Captures().Snapshot().retiring == 1 && pool.InUse() == 1,
		"Disconnect destroyed a pending GPU capture before its producer completed");
	output->status.store(CompletionStatus::Complete);
	session.Captures().Poll();
	Require(session.Captures().Snapshot().retiring == 0 && reader->output->PollCompletion() == CompletionStatus::Complete,
		"Completed retired capture failed to relinquish store ownership");
	session.Close();
	session.Display().PollRetired();
	output.reset();
	Require(reader->output && reader->metadata->key == first && pool.InUse() == 1,
		"Sequence retirement invalidated an independent image reader");
	reader.reset();
	Require(pool.InUse() == 0, "Sequence capture producer survived its final reader");
}
void TestSequenceStopReleasesHeldInput()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(984), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(985), instance);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
		"Begin sequence input stop test");
	PoseSequenceStep step;
	step.controllers[0] = ControllerPosePatch{};
	step.controllers[0]->trigger = .8f;
	const CommandSource source{CommandSourceKind::Mcp, 89};
	session.Commands().StartPoseSequence(source, std::make_shared<const PoseSequence>(std::vector{step}, true));
	FramePrediction frame;
	Require(session.WaitFrame(frame) == XR_SUCCESS && session.Input().Snapshot().controllers[0].trigger == .8f,
		"Sequence did not publish held input");
	session.Scheduler().Pause();
	session.StopPoseSequence(source);
	Require(session.Input().Snapshot().controllers[0].trigger == 0 && session.Snapshot().lastIssued.id == frame.id,
		"Sequence stop required another admitted frame to release held input");
}

void TestSequenceCaptureUnsupportedAndOwnership()
{
	using namespace simxr;
	FrameHistory history;
	const auto generation = history.StartGeneration();
	SessionCaptureCoordinator captures(history);
	FramePool pool(2);
	Require(captures.Configure(FramePool(pool), false, {}, false, false) == XR_SUCCESS,
		"Configure shared frame captures");
	captures.Start(generation);
	const CommandSource owner{CommandSourceKind::Mcp, 73};
	const std::array targets{CaptureTarget{}};
	const auto manual = captures.ReserveBundle(targets);
	Require(manual.result == SnapshotAdmission::Accepted && manual.captures.size() == 1,
		"Reserve independent manual capture");
	const SequenceCaptureIntent request{9, {1}, owner};
	CaptureId sequenceId;
	const FramePrediction prediction{{1}, {1}, {Duration{100}}, Duration{10}};
	{
		auto sequence = captures.PrepareSequenceCapture(request);
		sequenceId = sequence.Record()->reservation.id;
		history.Issue(prediction,
			[&](auto issued, auto cancelled)
			{
				captures.BindIssued(issued, cancelled);
				sequence.Bind(issued, cancelled);
			}, sequence.Record());
	}
	captures.Snapshots().RetireSequenceCaptures(owner);
	Require(!captures.Snapshots().Read(sequenceId) && captures.Snapshots().Read(manual.captures.front()) && pool.InUse() == 1,
		"Sequence cleanup retired another owner's capture of the same frame");
	FrameHistory unsupportedHistory;
	SessionCaptureCoordinator unsupported(unsupportedHistory);
	const auto unsupportedGeneration = unsupportedHistory.StartGeneration();
	unsupported.Start(unsupportedGeneration);
	auto rejected = unsupported.PrepareSequenceCapture(request);
	Require(rejected.Record()->reservation.result == SnapshotAdmission::Unsupported &&
		!rejected.Record()->reservation.id.value, "Unsupported capture was reported as accepted");
	unsupportedHistory.Issue(prediction,
		[&](auto issued, auto cancelled)
		{
			rejected.Bind(issued, cancelled);
		}, rejected.Record());
	Require(unsupportedHistory.Find({unsupportedGeneration, {1}})->sequenceCapture->reservation.result == SnapshotAdmission::Unsupported,
		"Unsupported sequence capture lost its exact-frame result");
}

void TestRejectedOutputDestruction()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(76), {}, XR_MAKE_VERSION(1, 0, 27));
	for (bool full : {true, false})
	{
		auto owner = std::make_unique<Session>(reinterpret_cast<XrSession>(77), instance);
		auto& session = *owner;
		FrameSchedulerConfig config;
		config.mode = ExecutionMode::Deterministic;
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session.Ready(config.epoch) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS &&
					session.Display().Pause(),
				"Begin rejected-output session");
		FramePrediction prediction;
		if (full)
			for (std::size_t index = 0; index < session.Display().Snapshot().capacity; ++index)
				Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
							session.EndFrame(prediction) == XR_SUCCESS,
						"Fill paused presentation budget");
		Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
				"Issue rejected output");
		auto output = std::make_shared<CaptureOutput>(nullptr);
		bool destroyed = false;
		output->destroy = [&]
		{
			session.Snapshot();
			session.SupportsCapture();
			session.Captures().Snapshot();
			destroyed = true;
		};
		auto layers = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
														  std::vector<LayerDescription>{QuadLayerDescription{}});
		const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,		   nullptr, prediction.displayTime.value.count(),
								 XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		nullptr};
		bool submitted = false;
		const auto result = session.EndFrame(prediction, &end, layers, std::move(output),
											 [&]
											 {
												 submitted = true;
												 return XR_ERROR_OUT_OF_MEMORY;
											 });
		Require(result == (full ? XR_ERROR_LIMIT_REACHED : XR_ERROR_OUT_OF_MEMORY) && destroyed && !output &&
					submitted == !full && session.Scheduler().Snapshot().begun->id == prediction.id,
				"Rejected last output reader was destroyed under the session mutex or consumed the frame");
	}
}

void TestCaptureConfigurationOwnership()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(82), {}, XR_MAKE_VERSION(1, 0, 27));
	for (bool accepted : {false, true})
	{
		auto owner = std::make_unique<Session>(reinterpret_cast<XrSession>(83), instance);
		auto* session = owner.get();
		FramePool pool(1);
		if (!accepted)
			Require(session->ConfigureCapturePool(pool) == XR_SUCCESS, "Configure first capture pool");
		const auto expectedCapacity = session->Captures().Snapshot().capacity;
		bool destroyed = false;
		auto reader = std::make_shared<CaptureOutput>(nullptr);
		reader->destroy = [&]
		{
			const auto state = session->Snapshot();
			Require(state.closed == accepted && session->SupportsCapture() == !accepted &&
						session->Captures().Snapshot().capacity == expectedCapacity,
					"Configuration's last reader reentered an inconsistent or locked capture owner");
			destroyed = true;
		};
		const auto configured =
			session->ConfigureCapturePool(pool, false,
										  [reader = std::move(reader)](const auto&, const auto&, auto)
										  {
											  return SnapshotAdmission::Accepted;
										  });
		Require(configured == (accepted ? XR_SUCCESS : XR_ERROR_CALL_ORDER_INVALID) && destroyed == !accepted,
				"Rejected configuration destroyed its callback under caller/owner locks or accepted config lost it");
		owner.reset();
		Require(destroyed, "Capture coordinator retained callback resources after teardown");
	}
}

void TestCaptureReservations()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(70), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(71), instance);
	unsigned reclamations = 0;
	FramePool pool(1, [&] { ++reclamations; });
	Require(session.ConfigureCapturePool(pool) == XR_SUCCESS &&
				session.ConfigureCapturePool(pool) == XR_ERROR_CALL_ORDER_INVALID,
			"Capture pool configuration lifetime");
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	config.period = Duration{10};
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(config.epoch) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Capture session begin failed");
	Require(session.ReserveCapture().result == SnapshotAdmission::Invalid,
			"Unpaused session reserved a strict capture");
	Require(reclamations == 0, "Unpaused capture request reclaimed retained snapshots");
	session.Scheduler().Pause();
	const std::array duplicateTargets{CaptureTarget{}, CaptureTarget{}};
	Require(session.ReserveCaptureBundle(duplicateTargets).result == SnapshotAdmission::Invalid && reclamations == 0,
		"Duplicate capture targets reclaimed retained snapshots before rejection");
	Require(session.Display().Pause(), "Capture display pause failed");
	Require(session.ReserveCapture({CaptureKind::LayerView, 0, 0}).result == SnapshotAdmission::Unsupported,
			"Unsupported raw source capture silently selected composed eyes");
	const auto capture = session.ReserveCapture();
	Require(capture.result == SnapshotAdmission::Accepted && pool.InUse() == 1 &&
				session.ReserveCapture().result == SnapshotAdmission::Capacity &&
				session.Scheduler().Snapshot().permits == 0,
			"Capture capacity was not reserved before admission");
	FramePrediction prediction;
	Require(session.Scheduler().GrantPermits() == XR_SUCCESS && session.WaitFrame(prediction) == XR_SUCCESS,
			"Reserved capture admission failed");
	const auto key = FrameKey{session.Snapshot().generation, prediction.id};
	Require(session.Captures().Read(capture.id)->frame == key &&
				session.Captures().Read(capture.id)->metadata == session.History().Find(key) &&
				session.BeginFrame() == XR_SUCCESS &&
				session.Captures().Read(capture.id)->phase == SnapshotPhase::Begun,
			"Capture did not bind the exact issued/begun frame");
	auto token = session.Captures().ProducerReservation(key);
	Require(token && pool.Consume(token), "Prepared capture did not receive its actual producer reservation");
	auto output = std::make_shared<CaptureOutput>(std::move(token));
	const auto prepared = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
															  std::vector<LayerDescription>{QuadLayerDescription{}});
	const XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO,			nullptr, prediction.displayTime.value.count(),
							  XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		 nullptr};
	Require(session.EndFrame(prediction, &info, prepared, output,
							 []
							 {
								 return XR_ERROR_OUT_OF_MEMORY;
							 }) == XR_ERROR_OUT_OF_MEMORY &&
				session.Captures().Read(capture.id)->phase == SnapshotPhase::Begun &&
				session.Scheduler().Snapshot().begun->id == key.frame,
			"Rejected GPU admission consumed captured frame metadata");
	Require(session.EndFrame(prediction, &info, prepared, output,
							 []
							 {
								 return XR_SUCCESS;
							 }) == XR_SUCCESS,
			"Reserved capture submission retry failed");
	session.Captures().Poll();
	Require(session.Captures().Read(capture.id)->phase == SnapshotPhase::GpuPending,
			"Captured submission fabricated producer completion");
	output->status.store(CompletionStatus::Complete);
	output.reset();
	session.Captures().Poll();
	auto reader = session.Captures().Read(capture.id);
	const auto metadata = reader->metadata;
	Require(reader->phase == SnapshotPhase::Ready && metadata == session.History().Find(key) &&
				metadata->submission->layers == prepared,
			"Captured GPU readiness lost immutable CPU metadata");
	session.Display().Step();
	Require(session.Display().Step()->source == key, "Explicit display step did not select the captured output");
	for (int index = 0; index < 140; ++index)
	{
		Require(session.Scheduler().GrantPermits() == XR_SUCCESS && session.WaitFrame(prediction) == XR_SUCCESS &&
					session.BeginFrame() == XR_SUCCESS && session.EndFrame(prediction) == XR_SUCCESS,
				"Later frames failed while an exact snapshot was retained");
		session.Display().Step();
	}
	Require(!session.History().Find(key) && session.Captures().Read(capture.id)->metadata == metadata &&
				reader->output && reader->frame == key,
			"History eviction or source progression changed a retained snapshot");
	Require(session.Captures().Release(capture.id) && pool.InUse() == 1,
			"Release ignored a retained snapshot reader's producer ownership");
	reader.reset();
	Require(pool.InUse() == 0, "Last snapshot reader did not return producer capacity");
	const auto clear = session.ReserveCapture();
	Require(clear.result == SnapshotAdmission::Accepted && session.Scheduler().GrantPermits() == XR_SUCCESS &&
				session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
				session.EndFrame(prediction) == XR_SUCCESS &&
				session.Captures().Read(clear.id)->phase == SnapshotPhase::Clear && pool.InUse() == 0,
			"Clear capture retained unused native capacity or substituted an image");
	const auto unused = session.ReserveCapture();
	const auto before = session.Snapshot().lastIssued.id;
	Require(unused.result == SnapshotAdmission::Accepted && session.Scheduler().GrantPermits() == XR_SUCCESS &&
				session.CancelCaptureReservations() == 1 && session.Scheduler().Snapshot().permits == 0 &&
				pool.InUse() == 0 && !session.Captures().Read(unused.id) && session.Snapshot().lastIssued.id == before,
			"Cancelling unused capture permits issued extra work or retained budget");
	const auto discarded = session.ReserveCapture();
	Require(discarded.result == SnapshotAdmission::Accepted && session.Scheduler().GrantPermits() == XR_SUCCESS &&
				session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS &&
				session.Scheduler().GrantPermits() == XR_SUCCESS && session.WaitFrame(prediction) == XR_SUCCESS &&
				session.BeginFrame() == XR_FRAME_DISCARDED &&
				session.Captures().Read(discarded.id)->phase == SnapshotPhase::Discarded &&
				session.Captures().Read(discarded.id)->metadata->stage == FrameStage::Discarded && pool.InUse() == 0,
			"Discarded captured frame retained a producer permit or an old image");
	session.Close();
	Require(session.Captures().Snapshot().captures.empty() && pool.InUse() == 0,
			"Session close left logical captures or unused producers active");
}

void TestCaptureBoundary()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(72), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(73), instance);
	FramePool pool(2);
	Require(!session.SupportsCapture() && session.ConfigureCapturePool(pool, false, {}, false, true) == XR_SUCCESS && session.SupportsCapture(),
			"Actual native capture capability was not reported");
	Require(session.SupportsCapture({CaptureKind::ProjectionLayers}) && session.SupportsCapture({CaptureKind::QuadLayers}) &&
			!session.SupportsCapture({CaptureKind::LayerView}) && !session.SupportsCapture({CaptureKind::LayerDepth}),
			"Filtered composition support incorrectly depends on or grants raw image capture");
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	config.period = Duration{10};
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(config.epoch) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Boundary session begin failed");
	FramePrediction prediction;
	Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
			"Boundary initial prediction failed");
	auto token = pool.Reserve();
	Require(pool.Consume(token), "Normal producer reservation failed");
	auto output = std::make_shared<CaptureOutput>(std::move(token));
	output->status.store(CompletionStatus::Complete);
	const auto prepared = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
															  std::vector<LayerDescription>{QuadLayerDescription{}});
	const XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO,			nullptr, prediction.displayTime.value.count(),
							  XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		 nullptr};
	Require(session.EndFrame(prediction, &info, prepared, output) == XR_SUCCESS, "Boundary initial output failed");
	const FrameKey first{session.Snapshot().generation, prediction.id};
	std::weak_ptr<const FrameRecord> historical = session.History().Find(first);
	output.reset();
	session.Scheduler().Pause();
	Require(session.Display().Pause() && session.Display().Step()->source == first,
			"Boundary initial display selection failed");
	for (int index = 0; index < 140; ++index)
	{
		Require(session.Scheduler().GrantPermits() == XR_SUCCESS && session.WaitFrame(prediction) == XR_SUCCESS,
				"Boundary diagnostic frame admission failed");
		const auto begun = session.BeginFrame();
		Require(begun == (index ? XR_FRAME_DISCARDED : XR_SUCCESS), "Boundary diagnostic discard result changed");
	}
	Require(!session.History().Find(first) && !historical.expired(),
			"Selected display output did not retain source CPU metadata beyond history eviction");
	const auto boundary = session.CaptureBoundary();
	Require(boundary.result == SnapshotAdmission::Accepted && boundary.captures.size() == 2 && pool.InUse() == 2,
			"Boundary did not pin current output plus already-begun work");
	const auto duplicate = session.CaptureBoundary();
	Require(duplicate.result == SnapshotAdmission::Accepted && duplicate.captures == boundary.captures &&
				pool.InUse() == 2 && session.Scheduler().Snapshot().permits == 0,
			"Repeated boundary capture duplicated reservations or issued work");
	session.Captures().Poll();
	auto selected = session.Captures().Read(boundary.captures.front());
	const auto pending = session.Captures().Read(boundary.captures.back());
	Require(selected->phase == SnapshotPhase::Ready && selected->metadata == historical.lock() &&
				selected->frame == first && pending->phase == SnapshotPhase::Begun &&
				pending->frame->frame == prediction.id,
			"Boundary capture substituted latest metadata for a retained old output");
	session.Close();
	Require(!session.SupportsCapture() && !session.Captures().Read(boundary.captures.front()) && pool.InUse() == 1,
			"Close failed to retire logical boundary capture or ignored a live reader");
	selected.reset();
	Require(pool.InUse() == 0, "Released boundary reader retained native producer capacity");

	Session constrained(reinterpret_cast<XrSession>(74), instance);
	FramePool smallPool(1);
	Require(constrained.ConfigureCapturePool(smallPool) == XR_SUCCESS &&
				constrained.Ready(config.epoch) == XR_SUCCESS && constrained.Begin(begin, config) == XR_SUCCESS &&
				constrained.WaitFrame(prediction) == XR_SUCCESS && constrained.BeginFrame() == XR_SUCCESS,
			"Constrained boundary session failed");
	token = smallPool.Reserve();
	Require(smallPool.Consume(token), "Constrained normal producer failed");
	output = std::make_shared<CaptureOutput>(std::move(token));
	output->status.store(CompletionStatus::Complete);
	Require(constrained.EndFrame(prediction, &info, prepared, output) == XR_SUCCESS,
			"Constrained initial output failed");
	output.reset();
	constrained.Scheduler().Pause();
	Require(constrained.Display().Pause() && constrained.Display().Step().has_value() &&
				constrained.Scheduler().GrantPermits() == XR_SUCCESS &&
				constrained.WaitFrame(prediction) == XR_SUCCESS && constrained.BeginFrame() == XR_SUCCESS,
			"Constrained boundary work was not admitted");
	const auto outstanding = constrained.Snapshot().lastIssued.id;
	const auto failed = constrained.CaptureBoundary();
	Require(failed.result == SnapshotAdmission::Capacity && failed.captures.empty() &&
				constrained.Snapshot().lastIssued.id == outstanding && constrained.Scheduler().Snapshot().permits == 0,
			"Boundary capacity failure reported success or silently granted lookahead");
	Require(constrained.EndFrame(prediction) == XR_SUCCESS && constrained.Display().Step().has_value(),
			"Already-admitted work could not drain after a retention failure");
	constrained.Captures().Poll();
	const auto retried = constrained.CaptureBoundary();
	Require(retried.result == SnapshotAdmission::Accepted && smallPool.InUse() == 0 && retried.captures.size() == 1 &&
				constrained.Captures().Read(retried.captures.front())->phase == SnapshotPhase::Clear,
			"Retention failure could not recover after actual owner release");
	constrained.Close();

	Session unsupported(reinterpret_cast<XrSession>(75), instance);
	Require(unsupported.Ready(config.epoch) == XR_SUCCESS && unsupported.Begin(begin, config) == XR_SUCCESS &&
				unsupported.WaitFrame(prediction) == XR_SUCCESS && unsupported.BeginFrame() == XR_SUCCESS,
			"Unconfigured capture session failed");
	output = std::make_shared<CaptureOutput>(nullptr);
	output->status.store(CompletionStatus::Complete);
	Require(unsupported.EndFrame(prediction, &info, prepared, output) == XR_SUCCESS,
			"Unconfigured session could not compose an ordinary output");
	unsupported.Scheduler().Pause();
	Require(unsupported.Display().Pause() && unsupported.Display().Step().has_value(),
			"Unconfigured display could not select an ordinary output");
	const auto unavailable = unsupported.CaptureBoundary();
	Require(!unsupported.SupportsCapture() && unavailable.result == SnapshotAdmission::Unsupported &&
				unavailable.captures.empty() && unsupported.Captures().Snapshot().captures.empty(),
			"Boundary capture bypassed unavailable native inspection capability");
	unsupported.Close();
}

void TestLayerViewCapture()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(180), {}, XR_MAKE_VERSION(1, 0, 27));
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	const CaptureTarget outer{CaptureKind::LayerView, 0, 0}, inset{CaptureKind::LayerView, 0, 3},
		quad{CaptureKind::LayerView, 1, 0};
	{
		Session small(reinterpret_cast<XrSession>(181), instance);
		FramePool pool(1);
		Require(small.ConfigureCapturePool(pool, true) == XR_SUCCESS && small.Ready(timing.epoch) == XR_SUCCESS &&
					small.Begin(begin, timing) == XR_SUCCESS,
				"Begin small raw pool");
		small.Scheduler().Pause();
		small.Display().Pause();
		Require(small.ReserveCapture(inset).result == SnapshotAdmission::Capacity && pool.InUse() == 0 &&
					small.Captures().Snapshot().captures.empty(),
				"Raw admission issued without composed companion capacity");
	}
	for (const auto target : {outer, inset, quad})
	{
		Session session(reinterpret_cast<XrSession>(182), instance);
		FramePool pool(4);
		Require(session.ConfigureCapturePool(pool, true) == XR_SUCCESS && session.SupportsCapture(inset) &&
					session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
				"Begin selected-layer capture");
		session.Scheduler().Pause();
		session.Display().Pause();
		const auto unused = session.ReserveCapture(target);
		Require(unused.result == SnapshotAdmission::Accepted && pool.InUse() == 2 &&
					session.Scheduler().GrantPermits() == XR_SUCCESS && session.CancelCaptureReservations() == 1 &&
					pool.InUse() == 0 && !session.Captures().Read(unused.id) &&
					session.Scheduler().Snapshot().permits == 0,
				"Cancel unused raw permit retained one of its producer budgets");
		const auto capture = session.ReserveCapture(target);
		Require(capture.result == SnapshotAdmission::Accepted && pool.InUse() == 2 &&
					session.Scheduler().GrantPermits() == XR_SUCCESS,
				"Raw admission did not reserve both outputs before permit");
		FramePrediction prediction;
		Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS, "Issue raw target");
		const FrameKey key{session.Snapshot().generation, prediction.id};
		Require(session.Captures().RequestedTargets(key) == std::vector<CaptureTarget>{target},
				"Native target query lost selected identity");
		std::shared_ptr<FrameReservation> rawToken, composedToken;
		Require(session.PrepareCaptureProducer(key, rawToken, target) == XR_SUCCESS &&
					session.PrepareCaptureProducer(key, composedToken) == XR_SUCCESS && rawToken && composedToken &&
					rawToken != composedToken,
				"Raw and composed preparation shared one budget");
		Require(pool.Consume(rawToken) && pool.Consume(composedToken), "Consume raw/composed tokens");
		auto raw = std::make_shared<CaptureOutput>(std::move(rawToken));
		auto composed = std::make_shared<CaptureOutput>(std::move(composedToken));
		raw->status.store(CompletionStatus::Complete);
		composed->status.store(CompletionStatus::Complete);
		ProjectionLayerDescription projection;
		projection.views.resize(4);
		auto metadata =
			std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
												std::vector<LayerDescription>{projection, QuadLayerDescription{}});
		const XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO,			nullptr, prediction.displayTime.value.count(),
								  XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 2,		 nullptr};
		std::array outputs{simxr::CaptureOutput{target, raw}};
		Require(session.EndFrame(
					prediction, &info, metadata, composed,
					[]
					{
						return XR_SUCCESS;
					},
					outputs) == XR_SUCCESS,
				"Submit exact raw target");
		session.Captures().Poll();
		const auto record = session.Captures().Read(capture.id);
		Require(record && record->phase == SnapshotPhase::Ready && record->target == target && record->output == raw &&
					record->output != composed && record->metadata->submission->layers == metadata &&
					session.Captures().RequestedTargets(key).empty(),
				"Raw snapshot substituted composed output or lost metadata");
		Require(session.Captures().Release(capture.id), "Release selected target");
		session.Close();
		// Each independent target also exercises retained-reader ownership at close.
		Require(pool.InUse() == 2, "Closing session erased active raw/composed readers");
	}
}

void TestLayerCaptureRejectionReentry()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(185), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(186), instance);
	FramePool pool(2);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.ConfigureCapturePool(pool, true) == XR_SUCCESS && session.Ready(timing.epoch) == XR_SUCCESS &&
				session.Begin(begin, timing) == XR_SUCCESS,
			"Begin raw rejection test");
	session.Scheduler().Pause();
	session.Display().Pause();
	const CaptureTarget target{CaptureKind::LayerView, 0, 0};
	const auto capture = session.ReserveCapture(target);
	FramePrediction prediction;
	Require(capture.result == SnapshotAdmission::Accepted && session.Scheduler().GrantPermits() == XR_SUCCESS &&
				session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
			"Issue raw rejection test");
	const FrameKey key{session.Snapshot().generation, prediction.id};
	auto raw = std::make_shared<CaptureOutput>(session.Captures().ProducerReservation(key, target));
	auto composed = std::make_shared<CaptureOutput>(session.Captures().ProducerReservation(key));
	bool destroyed = false;
	raw->destroy = [&]
	{
		session.Snapshot();
		destroyed = true;
	};
	std::array outputs{simxr::CaptureOutput{target, raw}};
	raw.reset();
	ProjectionLayerDescription projection;
	projection.views.resize(2);
	auto metadata = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
														std::vector<LayerDescription>{projection});
	const XrFrameEndInfo info{XR_TYPE_FRAME_END_INFO,			nullptr, prediction.displayTime.value.count(),
							  XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		 nullptr};
	std::vector<simxr::CaptureOutput> oversized(session.Captures().Capacity() + 1, {target, {}});
	Require(session.EndFrame(prediction, &info, metadata, composed, {}, oversized) == XR_ERROR_VALIDATION_FAILURE &&
				session.Scheduler().Snapshot().begun &&
				session.Captures().Read(capture.id)->phase == SnapshotPhase::Begun,
			"Unbounded raw output array was copied or consumed the begun frame");
	Require(session.EndFrame(
				prediction, &info, metadata, composed,
				[&]
				{
					outputs[0].output.reset();
					return XR_ERROR_RUNTIME_FAILURE;
				},
				outputs) == XR_ERROR_RUNTIME_FAILURE &&
				destroyed && session.Scheduler().Snapshot().begun &&
				session.Captures().Read(capture.id)->phase == SnapshotPhase::Begun,
			"Rejected raw submission destroyed last reader under Session lock or consumed frame");
	session.Captures().AbandonProducer(key, target);
	std::shared_ptr<FrameReservation> replacement;
	Require(session.PrepareCaptureProducer(key, replacement, target) == XR_SUCCESS && replacement &&
				session.Captures().ProducerReservation(key),
			"Target retry lost composed companion");
	Require(session.Captures().Release(capture.id), "Release raw request during native retry");
	replacement.reset();
	Require(session.PrepareCaptureProducer(key, replacement, target) == XR_SUCCESS && !replacement &&
				session.Captures().RequestedTargets(key).empty(),
			"Released raw request acquired hidden native capacity");
	session.Close();
}

void TestLiveSubmissionPolicy()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(190), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(191), instance);
	FramePool debug(4);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.ConfigureCapturePool(debug) == XR_SUCCESS && session.Ready(timing.epoch) == XR_SUCCESS &&
				session.Begin(begin, timing) == XR_SUCCESS,
			"Begin live submission test");
	session.Scheduler().Pause();
	session.Display().Pause();
	auto issue = [&]
	{
		FramePrediction frame;
		Require(session.Scheduler().GrantPermits() == XR_SUCCESS && session.WaitFrame(frame) == XR_SUCCESS &&
					session.BeginFrame() == XR_SUCCESS,
				"Issue live frame");
		return frame;
	};
	auto layers = [](const FramePrediction& frame)
	{
		return std::make_shared<const FrameLayers>(frame.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
												   std::vector<LayerDescription>{QuadLayerDescription{}});
	};
	auto info = [](const FramePrediction& frame)
	{
		return XrFrameEndInfo{XR_TYPE_FRAME_END_INFO,			nullptr, frame.displayTime.value.count(),
							  XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		 nullptr};
	};
	unsigned nativeCalls = 0;
	auto submit = [&]
	{
		++nativeCalls;
		return XR_SUCCESS;
	};
	auto first = issue();
	auto selected = std::make_shared<CaptureOutput>(nullptr);
	selected->status.store(CompletionStatus::Complete);
	const auto firstInfo = info(first);
	Require(session.EndFrame(first, &firstInfo, layers(first), selected, submit) == XR_SUCCESS,
			"Submit initial selected source");
	while (session.Display().NextTick() <= first.displayTick)
		session.Display().Step();
	const FrameKey selectedKey{session.Snapshot().generation, first.id};
	std::vector<std::shared_ptr<CaptureOutput>> pending;
	for (unsigned index = 0; index < session.Display().Snapshot().capacity; ++index)
	{
		auto frame = issue();
		auto end = info(frame);
		auto output = std::make_shared<CaptureOutput>(nullptr);
		Require(session.EndFrame(frame, &end, layers(frame), output, submit) == XR_SUCCESS,
				"Fill pending presentation queue");
		pending.push_back(std::move(output));
	}
	const auto acceptedNativeCalls = nativeCalls;
	auto nativeDrop = issue();
	auto nativeInfo = info(nativeDrop);
	Require(session.EndFrame(nativeDrop, &nativeInfo, layers(nativeDrop), {}, submit, {},
							 FrameSubmissionPolicy::Live) == XR_SUCCESS &&
				nativeCalls == acceptedNativeCalls,
			"Native-capacity live drop submitted work or retained begun frame");
	const auto nativeRecord = session.History().Find({selectedKey.generation, nativeDrop.id});
	Require(nativeRecord->submission->outputDisposition == FrameOutputDisposition::Dropped &&
				nativeRecord->submission->dropReason == FrameDropReason::NativeCapacity,
			"Native-capacity drop metadata missing");
	auto presentationDrop = issue();
	auto presentationInfo = info(presentationDrop);
	bool destroyed = false;
	auto dropped = std::make_shared<CaptureOutput>(nullptr);
	dropped->destroy = [&]
	{
		session.Snapshot();
		destroyed = true;
	};
	const auto droppedResult = session.EndFrame(presentationDrop, &presentationInfo, layers(presentationDrop),
												std::move(dropped), submit, {}, FrameSubmissionPolicy::Live);
	Require(droppedResult == XR_SUCCESS && nativeCalls == acceptedNativeCalls + 1 && destroyed,
			"Display drop skipped history execution or destroyed last output under Session lock");
	const auto presentationRecord = session.History().Find({selectedKey.generation, presentationDrop.id});
	Require(presentationRecord->submission->outputDisposition == FrameOutputDisposition::Dropped &&
				presentationRecord->submission->dropReason == FrameDropReason::PresentationCapacity &&
				session.Display().Snapshot().latest->source == selectedKey,
			"Live drop changed selected display source or lacked reason");
	const auto abandoned = session.ReserveCapture();
	auto abandonedFrame = issue();
	auto abandonedInfo = info(abandonedFrame);
	Require(abandoned.result == SnapshotAdmission::Accepted && session.Captures().Release(abandoned.id),
			"Release admitted capture before live drop");
	auto abandonedOutput = std::make_shared<CaptureOutput>(nullptr);
	Require(session.EndFrame(abandonedFrame, &abandonedInfo, layers(abandonedFrame),
							 abandonedOutput, submit, {},
							 FrameSubmissionPolicy::Live) == XR_SUCCESS,
			"Drop released capture frame");
	session.Captures().Poll();
	Require(session.Captures().Snapshot().retiring != 0, "Display drop retired pending native work");
	abandonedOutput->status.store(CompletionStatus::Complete);
	session.Captures().Poll();
	Require(session.Captures().Snapshot().retiring == 0 && debug.InUse() == 0,
			"Display drop retained completed output in abandoned capture");
	const auto exactCapture = session.ReserveCapture();
	auto exact = issue();
	auto exactInfo = info(exact);
	Require(exactCapture.result == SnapshotAdmission::Accepted &&
				session.EndFrame(exact, &exactInfo, layers(exact), {}, submit, {}, FrameSubmissionPolicy::Live) ==
					XR_ERROR_LIMIT_REACHED &&
				session.Scheduler().Snapshot().begun->id == exact.id &&
				session.History().Find({selectedKey.generation, exact.id})->stage == FrameStage::Begun,
			"Requested capture silently accepted live native-capacity drop");
	auto exactOutput = std::make_shared<CaptureOutput>(session.Captures().ProducerReservation(exactCapture.id));
	Require(session.EndFrame(exact, &exactInfo, layers(exact), exactOutput, submit, {}, FrameSubmissionPolicy::Live) ==
					XR_ERROR_LIMIT_REACHED &&
				nativeCalls == acceptedNativeCalls + 2 &&
				session.Captures().Read(exactCapture.id)->phase == SnapshotPhase::Begun,
			"Capture queue pressure consumed begun frame or callback");
	pending.front()->status.store(CompletionStatus::Complete);
	while (session.Display().NextTick() <= exact.displayTick)
		session.Display().Step();
	Require(session.EndFrame(exact, &exactInfo, layers(exact), exactOutput, submit, {}, FrameSubmissionPolicy::Live) ==
				XR_SUCCESS,
			"Exact capture could not retry after presentation capacity returned");
	session.Captures().Poll();
	Require(session.Captures().Read(exactCapture.id)->phase == SnapshotPhase::GpuPending &&
				session.History().Find({selectedKey.generation, exact.id})->submission->outputDisposition ==
					FrameOutputDisposition::Submitted,
			"Submitted CPU disposition fabricated GPU completion");
	Require(session.Captures().Release(exactCapture.id), "Release pending capture before clear");
	const auto exactClear = session.ReserveCapture();
	auto clear = issue();
	Require(exactClear.result == SnapshotAdmission::Accepted &&
				session.EndFrame(clear, nullptr, {}, {}, {}, {}, FrameSubmissionPolicy::Live) ==
					XR_ERROR_LIMIT_REACHED &&
				!session.Display().Snapshot().liveClear &&
				session.Captures().Read(exactClear.id)->phase == SnapshotPhase::Begun,
			"Requested zero-layer capture silently used the live clear mailbox");
	Require(session.Captures().Release(exactClear.id), "Abandon exact clear retention without consuming its frame");
	Require(session.EndFrame(clear, nullptr, {}, {}, {}, {}, FrameSubmissionPolicy::Live) == XR_SUCCESS &&
				session.Display().Snapshot().liveClear == FrameKey{selectedKey.generation, clear.id},
			"Live clear blocked behind full pending presentation queue");
	const auto clearRecord = session.History().Find({selectedKey.generation, clear.id});
	Require(clearRecord->submission->outputDisposition == FrameOutputDisposition::Clear &&
				clearRecord->submission->dropReason == FrameDropReason::None,
			"Clear recorded as dropped output");
	while (session.Display().NextTick() <= clear.displayTick)
		session.Display().Step();
	Require(session.Display().Snapshot().latest->kind == PresentationKind::Clear &&
				!session.Display().Snapshot().latest->output,
			"Live clear did not select explicit CPU marker");
	for (const auto& output : pending)
		output->status.store(CompletionStatus::Complete);
	exactOutput->status.store(CompletionStatus::Complete);
	session.Display().Step();
	session.Captures().Poll();
	Require(!session.Display().Snapshot().latest->output && session.Display().Snapshot().retiring.empty(),
			"Late GPU completion resurrected cleared live output");
	session.Close();
	exactOutput.reset();
	Require(debug.InUse() == 0, "Released capture retained admission capacity after real completion");
}

void TestQuadHistoryCaptureCapacity()
{
	using namespace simxr;
	const CaptureTarget rawTarget{CaptureKind::LayerView};
	const CaptureTarget quadTarget{CaptureKind::QuadLayers};
	const std::array bounds{XrRect2Di{{2, 3}, {64, 32}}, XrRect2Di{{4, 5}, {64, 32}}};
	for (const auto target : {rawTarget, quadTarget})
	{
		Instance instance(reinterpret_cast<XrInstance>(197), {}, XR_MAKE_VERSION(1, 0, 27));
		Session session(reinterpret_cast<XrSession>(198), instance);
		FramePool pool(2);
		Require(session.ConfigureCapturePool(
					pool, true,
					[](const auto& frame, const FramePool& budget, std::shared_ptr<FrameReservation> reserved)
					{
						const auto output = std::dynamic_pointer_cast<const CaptureOutput>(frame);
						Require(output && output->slot == reserved && budget.Consume(reserved),
								"Promote requested capture with its reserved producer");
						return SnapshotAdmission::Accepted;
					},
					false, true) == XR_SUCCESS &&
					session.EnableImageHistory() == XR_SUCCESS,
				"Configure quad history alongside manual capture");
		session.Captures().SetCapacity(1);
		FrameSchedulerConfig timing;
		timing.mode = ExecutionMode::Deterministic;
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
				"Begin quad history capacity session");
		session.Scheduler().Pause();
		session.Display().Pause();
		const auto capture = session.ReserveCapture(target);
		FramePrediction prediction;
		Require(capture.result == SnapshotAdmission::Accepted && session.Scheduler().GrantPermits() == XR_SUCCESS &&
					session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
				"Issue requested capture at snapshot capacity");
		const FrameKey key{session.Snapshot().generation, prediction.id};
		auto requested = std::make_shared<CaptureOutput>(session.Captures().ProducerReservation(key, target));
		auto composed = std::make_shared<CaptureOutput>(session.Captures().ProducerReservation(key));
		const auto& historyBudget = session.ImageHistory().Budget(ImageHistoryView::Quads);
		auto history = std::make_shared<CaptureOutput>(historyBudget.Reserve());
		Require(historyBudget.Consume(history->slot), "Reserve independent quad history budget");
		const auto extraTarget = target == rawTarget ? quadTarget : rawTarget;
		std::array outputs{simxr::CaptureOutput{target, requested, bounds},
						   simxr::CaptureOutput{extraTarget, history, bounds}};
		auto metadata = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
															std::vector<LayerDescription>{QuadLayerDescription{}});
		const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,		   nullptr, prediction.displayTime.value.count(),
								 XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		nullptr};
		unsigned submits = 0;
		auto submit = [&]
		{
			++submits;
			return XR_SUCCESS;
		};
		const std::array duplicates{simxr::CaptureOutput{quadTarget, {}, bounds},
									simxr::CaptureOutput{quadTarget, {}, bounds}};
		const std::array oversized{simxr::CaptureOutput{rawTarget, {}},
								   simxr::CaptureOutput{{CaptureKind::LayerView, 0, 1}, {}},
								   simxr::CaptureOutput{quadTarget, {}, bounds}};
		Require(session.EndFrame(prediction, &end, metadata, composed, submit, duplicates) ==
						XR_ERROR_VALIDATION_FAILURE &&
					session.EndFrame(prediction, &end, metadata, composed, submit, oversized) ==
						XR_ERROR_VALIDATION_FAILURE &&
					submits == 0 && session.Scheduler().Snapshot().begun && !requested->slot->Consumed(),
				"Optional history admitted duplicate or excessive capture outputs");
		if (target == quadTarget)
		{
			Require(session.EndFrame(prediction, &end, metadata, composed, submit, outputs) ==
							XR_ERROR_VALIDATION_FAILURE &&
						submits == 0 && session.Scheduler().Snapshot().begun &&
						session.Captures().Read(capture.id)->phase == SnapshotPhase::Begun &&
						!requested->slot->Consumed(),
					"Requested quad capture was excluded from ordinary capacity");
		}
		const auto accepted = std::span<const simxr::CaptureOutput>{outputs}.first(target == quadTarget ? 1 : 2);
		Require(session.EndFrame(prediction, &end, metadata, composed, submit, accepted) == XR_SUCCESS &&
					submits == 1 && !session.Scheduler().Snapshot().begun,
				"Independent quad history rejected requested capture at snapshot capacity");
		session.Captures().Poll();
		Require(session.Captures().Read(capture.id)->phase == SnapshotPhase::GpuPending &&
					session.Captures().Read(capture.id)->output == requested,
				"Optional history dropped requested output or fabricated completion");
		const auto retained = session.ImageHistory().Find(key, ImageHistoryView::Quads);
		Require(retained.metadata && retained.quadVisible && retained.quadBounds[0].extent.width == 64 &&
					retained.output == (target == rawTarget ? history : nullptr),
				"Quad history lost its output or retained a manually requested quad as automatic history");
		requested->status.store(CompletionStatus::Complete);
		composed->status.store(CompletionStatus::Complete);
		history->status.store(CompletionStatus::Complete);
		session.Captures().Poll();
		Require(session.Captures().Read(capture.id)->phase == SnapshotPhase::Ready &&
					session.Captures().Read(capture.id)->output == requested,
				"Requested capture did not become ready after real completion");
		Require(session.Captures().Release(capture.id), "Release manual capture beside quad history");
		session.Close();
	}
}

void TestQuadHistoryWithoutSnapshotCapacity()
{
	using namespace simxr;
	const auto previousBudget = SnapshotBudget::MiB();
	SnapshotBudget::Configure(SnapshotBudget::MinMiB);
	// Run last: the largest observed image allocation remains process-wide.
	SnapshotBudget::ObserveImageBytes(64 * 1024 * 1024);
	Require(SnapshotBudget::Capacity() == 0, "Large images exhaust exact snapshot admission");
	for (const bool historyEnabled : {false, true})
	{
		Instance instance(reinterpret_cast<XrInstance>(199), {}, XR_MAKE_VERSION(1, 0, 27));
		Session session(reinterpret_cast<XrSession>(200), instance);
		FramePool pool(2);
		Require(session.ConfigureCapturePool(
					pool, true,
					[](const auto&, const FramePool&, std::shared_ptr<FrameReservation>)
					{
						return SnapshotAdmission::Capacity;
					},
					false, true) == XR_SUCCESS &&
					(!historyEnabled || session.EnableImageHistory() == XR_SUCCESS),
				"Configure optional history with no exact snapshot capacity");
		FrameSchedulerConfig timing;
		timing.mode = ExecutionMode::Deterministic;
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session.Ready(timing.epoch) == XR_SUCCESS && session.Begin(begin, timing) == XR_SUCCESS,
				"Begin zero snapshot capacity session");
		session.Scheduler().Pause();
		session.Display().Pause();
		FramePrediction prediction;
		Require(session.Scheduler().GrantPermits() == XR_SUCCESS && session.WaitFrame(prediction) == XR_SUCCESS &&
					session.BeginFrame() == XR_SUCCESS,
				"Issue live frame without exact snapshot capacity");
		const FrameKey key{session.Snapshot().generation, prediction.id};
		auto composed = std::make_shared<CaptureOutput>(nullptr);
		composed->status.store(CompletionStatus::Complete);
		auto metadata = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
															std::vector<LayerDescription>{QuadLayerDescription{}});
		const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,		   nullptr, prediction.displayTime.value.count(),
								 XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		nullptr};
		const std::array bounds{XrRect2Di{{1, 2}, {64, 32}}, XrRect2Di{{3, 4}, {64, 32}}};
		const std::array quads{simxr::CaptureOutput{{CaptureKind::QuadLayers}, {}, bounds}};
		const std::array raw{simxr::CaptureOutput{{CaptureKind::LayerView}, {}}};
		unsigned submits = 0;
		auto submit = [&]
		{
			++submits;
			return XR_SUCCESS;
		};
		Require(session.EndFrame(prediction, &end, metadata, composed, submit, raw, FrameSubmissionPolicy::Live) ==
						XR_ERROR_VALIDATION_FAILURE &&
					submits == 0 && session.Scheduler().Snapshot().begun,
				"Zero snapshot capacity admitted an ordinary capture output");
		const auto result =
			session.EndFrame(prediction, &end, metadata, composed, submit, quads, FrameSubmissionPolicy::Live);
		if (historyEnabled)
		{
			Require(result == XR_SUCCESS && submits == 1 && !session.Scheduler().Snapshot().begun,
					"Optional quad history marker rejected live submission at zero snapshot capacity");
			const auto retained = session.ImageHistory().Find(key, ImageHistoryView::Quads);
			Require(retained.metadata && retained.metadata->stage == FrameStage::Submitted && !retained.output &&
						retained.quadVisible && retained.quadBounds[0].offset.x == 1 &&
						retained.quadBounds[0].extent.width == 64,
					"Missing optional image lost its exact live-frame history or bounds");
		}
		else
		{
			Require(result == XR_ERROR_VALIDATION_FAILURE && submits == 0 && session.Scheduler().Snapshot().begun,
					"Disabled image history widened capture admission");
			Require(session.EndFrame(prediction, &end, metadata, composed, submit, {}, FrameSubmissionPolicy::Live) ==
						XR_SUCCESS,
					"Rejected history marker consumed the live frame");
		}
		session.Close();
	}
	SnapshotBudget::Configure(previousBudget);
}

void TestCaptureBudgetPromotion()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(193), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(194), instance);
	FramePool live(1), debug(1);
	unsigned promotions = 0;
	Require(session.ConfigureCapturePool(
				debug, false,
				[&](const auto& frame, const FramePool& pool, std::shared_ptr<FrameReservation> reserved)
				{
					auto output =
						std::const_pointer_cast<CaptureOutput>(std::dynamic_pointer_cast<const CaptureOutput>(frame));
					if (!output)
						return SnapshotAdmission::Unsupported;
					if (pool.Owns(output->slot))
						return SnapshotAdmission::Accepted;
					auto replacement = reserved ? std::move(reserved) : pool.Reserve(false);
					if (!replacement)
						return SnapshotAdmission::Capacity;
					Require(pool.Consume(replacement), "Consume promoted budget");
					output->slot = std::move(replacement);
					++promotions;
					return SnapshotAdmission::Accepted;
				}) == XR_SUCCESS,
			"Configure CPU-only capture budget promoter");
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(config.epoch) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Begin budget promotion session");
	FramePrediction frame;
	Require(session.WaitFrame(frame) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS, "Issue live budget source");
	auto token = live.Reserve();
	Require(live.Consume(token), "Consume initial live budget");
	auto output = std::make_shared<CaptureOutput>(std::move(token));
	output->status.store(CompletionStatus::Complete);
	auto metadata = std::make_shared<const FrameLayers>(frame.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
														std::vector<LayerDescription>{QuadLayerDescription{}});
	const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,		   nullptr, frame.displayTime.value.count(),
							 XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		nullptr};
	Require(session.EndFrame(frame, &end, metadata, output, {}, {}, FrameSubmissionPolicy::Live) == XR_SUCCESS &&
				live.InUse() == 1,
			"Submit live-budget output");
	session.Scheduler().Pause();
	session.Display().Pause();
	const auto captured = session.CaptureBoundary();
	Require(captured.result == SnapshotAdmission::Accepted && captured.captures.size() == 1 && promotions == 1 &&
				live.InUse() == 0 && debug.InUse() == 1,
			"Existing live image was not promoted to capture budget");
	Require(session.CaptureBoundary().captures == captured.captures && promotions == 1,
			"Repeated boundary promoted existing debug owner twice");
	session.Close();
	session.Display().PollRetired();
	output.reset();
	Require(debug.InUse() == 0, "Promotion retained hidden capture budget after close");
}

void TestLateCaptureBudgetPromotion()
{
	using namespace simxr;
	Instance instance(reinterpret_cast<XrInstance>(195), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(196), instance);
	FramePool live(1), debug(1);
	unsigned promotions = 0;
	Require(session.ConfigureCapturePool(
				debug, false,
				[&](const auto& frame, const FramePool& pool, std::shared_ptr<FrameReservation> reserved)
				{
					auto output =
						std::const_pointer_cast<CaptureOutput>(std::dynamic_pointer_cast<const CaptureOutput>(frame));
					if (!output)
						return SnapshotAdmission::Unsupported;
					if (pool.Owns(output->slot))
						return SnapshotAdmission::Accepted;
					auto replacement = reserved ? std::move(reserved) : pool.Reserve(false);
					if (!replacement)
						return SnapshotAdmission::Capacity;
					Require(pool.Owns(replacement) && pool.Consume(replacement), "Consume exact late-promotion token");
					output->slot = std::move(replacement);
					++promotions;
					return SnapshotAdmission::Accepted;
				}) == XR_SUCCESS,
			"Configure late capture promotion");
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready(config.epoch) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Begin late promotion session");
	FramePrediction prediction;
	Require(session.WaitFrame(prediction) == XR_SUCCESS && session.BeginFrame() == XR_SUCCESS,
			"Issue live frame before pause race");
	const FrameKey key{session.Snapshot().generation, prediction.id};
	auto prepareLive = [&]
	{
		auto token = live.Reserve();
		Require(token && live.Consume(token), "Reserve live preparation slot");
		auto output = std::make_shared<CaptureOutput>(std::move(token));
		output->destroy = [&]
		{
			Require(session.Snapshot().generation == key.generation,
					"Late promotion destroyed output under Session lock");
		};
		return output;
	};
	auto output = prepareLive();
	session.Scheduler().Pause();
	Require(session.Display().Pause(), "Pause late promotion display");
	const auto boundary = session.CaptureBoundary();
	Require(boundary.result == SnapshotAdmission::Accepted && boundary.captures.size() == 1 && debug.InUse() == 1 &&
				live.InUse() == 1,
			"Pause boundary did not reserve pending frame before acceptance");
	auto original = session.Captures().ProducerReservation(key);
	Require(original && !original->Consumed(), "Boundary token was consumed before native preparation");
	auto metadata = std::make_shared<const FrameLayers>(prediction.displayTime, XR_ENVIRONMENT_BLEND_MODE_OPAQUE,
														std::vector<LayerDescription>{QuadLayerDescription{}});
	const XrFrameEndInfo end{XR_TYPE_FRAME_END_INFO,		   nullptr, prediction.displayTime.value.count(),
							 XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 1,		nullptr};
	unsigned submits = 0;
	const auto rejected = session.EndFrame(
		prediction, &end, metadata, output,
		[&]
		{
			++submits;
			Require(output->slot == original && original->Consumed() && live.InUse() == 0 && debug.InUse() == 1,
					"Late promotion allocated a hidden second slot or happened after native acceptance");
			return XR_ERROR_RUNTIME_FAILURE;
		},
		{}, FrameSubmissionPolicy::Live);
	Require(rejected == XR_ERROR_RUNTIME_FAILURE && submits == 1 && promotions == 1 &&
				!session.Captures().ProducerReservation(key) && session.Scheduler().Snapshot().begun &&
				session.Captures().Read(boundary.captures.front())->phase == SnapshotPhase::Begun,
			"Failed late promotion poisoned the capture's retry identity or consumed the begun frame");
	std::shared_ptr<FrameReservation> retry;
	Require(session.PrepareCaptureProducer(key, retry) == XR_ERROR_LIMIT_REACHED && !retry,
			"Failed output reader ceased to account for retained capacity");
	output.reset();
	// The observer above is deliberately another reservation reader.
	Require(debug.InUse() == 1, "Late promotion lost explicit reservation reader ownership");
	original.reset();
	Require(debug.InUse() == 0 && session.PrepareCaptureProducer(key, retry) == XR_SUCCESS && retry &&
				!retry->Consumed(),
			"Consumed failed-promotion token remained attached to Store");
	output = prepareLive();
	Require(session.EndFrame(
				prediction, &end, metadata, output,
				[&]
				{
					++submits;
					Require(output->slot == retry && retry->Consumed(),
							"Retry did not promote exact replacement token");
					return XR_SUCCESS;
				},
				{}, FrameSubmissionPolicy::Live) == XR_SUCCESS &&
				submits == 2 && promotions == 2 && live.InUse() == 0 && debug.InUse() == 1 &&
				!session.Scheduler().Snapshot().begun &&
				session.Captures().Read(boundary.captures.front())->phase == SnapshotPhase::GpuPending,
			"Late live-to-exact retry did not retain original capture without another permit");
	session.Captures().Poll();
	Require(session.Captures().Read(boundary.captures.front())->phase == SnapshotPhase::GpuPending,
			"Promotion fabricated GPU completion");
	output->status.store(CompletionStatus::Complete);
	session.Captures().Poll();
	Require(session.Captures().Read(boundary.captures.front())->phase == SnapshotPhase::Ready,
			"Late promoted output did not become ready after actual completion");
	session.Close();
	session.Display().PollRetired();
	retry.reset();
	output.reset();
	Require(debug.InUse() == 0 && live.InUse() == 0, "Late promotion retained hidden producer budget");
}

void TestInteractionDevices()
{
	using namespace simxr;
	const std::array enabled{XR_EXT_EYE_GAZE_INTERACTION_EXTENSION_NAME,
							 XR_HTCX_VIVE_TRACKER_INTERACTION_EXTENSION_NAME};
	Instance instance(reinterpret_cast<XrInstance>(40), enabled, XR_MAKE_VERSION(1, 0, 27));
	std::uint32_t count = 0;
	Require(instance.Trackers().Enumerate(0, &count, nullptr) == XR_SUCCESS && count == 4,
			"Instance tracker inventory before session");
	Session session(reinterpret_cast<XrSession>(41), instance);
	auto& registry = instance.Actions();
	auto path = [&](const char* name)
	{
		XrPath value{};
		Require(registry.StringToPath(name, value) == XR_SUCCESS, "Interaction path");
		return value;
	};
	XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(setInfo.actionSetName, "devices");
	std::strcpy(setInfo.localizedActionSetName, "Devices");
	XrActionSet set{};
	Require(registry.CreateSet(setInfo, set) == XR_SUCCESS, "Interaction set");
	auto action = [&](const char* name, XrActionType type, XrPath user)
	{
		XrActionCreateInfo info{XR_TYPE_ACTION_CREATE_INFO};
		std::strcpy(info.actionName, name);
		std::strcpy(info.localizedActionName, name);
		info.actionType = type;
		info.countSubactionPaths = 1;
		info.subactionPaths = &user;
		XrAction value{};
		Require(registry.CreateAction(set, info, value) == XR_SUCCESS, "Interaction action");
		return value;
	};
	const auto eyes = path("/user/eyes_ext");
	const auto waist = path("/user/vive_tracker_htcx/role/waist");
	const auto gaze = action("gaze", XR_ACTION_TYPE_POSE_INPUT, eyes);
	const auto tracker = action("tracker", XR_ACTION_TYPE_POSE_INPUT, waist);
	const auto vibration = action("pulse", XR_ACTION_TYPE_VIBRATION_OUTPUT, waist);
	const XrActionSuggestedBinding eyeBinding{gaze, path("/user/eyes_ext/input/gaze_ext/pose")};
	XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
												 path("/interaction_profiles/ext/eye_gaze_interaction"), 1,
												 &eyeBinding};
	Require(registry.SuggestBindings(suggest) == XR_SUCCESS, "Gaze binding");
	const std::array trackerBindings{
		XrActionSuggestedBinding{tracker, path("/user/vive_tracker_htcx/role/waist/input/grip/pose")},
		XrActionSuggestedBinding{vibration, path("/user/vive_tracker_htcx/role/waist/output/haptic")}};
	suggest.interactionProfile = path("/interaction_profiles/htc/vive_tracker_htcx");
	suggest.countSuggestedBindings = trackerBindings.size();
	suggest.suggestedBindings = trackerBindings.data();
	Require(registry.SuggestBindings(suggest) == XR_SUCCESS, "Tracker bindings");
	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
	Require(session.Attach(attach) == XR_SUCCESS, "Device attachment");
	XrPath profile{};
	Require(session.Actions().CurrentProfile(waist, profile) == XR_SUCCESS && profile == XR_NULL_PATH,
			"Tracker profile selected before synchronization");
	const auto initialEvents = instance.Events().Observe();
	Require(std::none_of(initialEvents.begin(), initialEvents.end(),
						 [](const auto& event)
						 {
							 return std::holds_alternative<InteractionProfileEvent>(event.event);
						 }),
			"Nonrunning attachment emitted profile event");
	const XrActiveActionSet beforeBeginActive{set, XR_NULL_PATH};
	const XrActionsSyncInfo beforeBeginSync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &beforeBeginActive};
	Require(session.SyncActions(beforeBeginSync) == XR_SESSION_NOT_FOCUSED, "Nonrunning device sync");
	const auto nonrunningEvents = instance.Events().Observe();
	Require(std::none_of(nonrunningEvents.begin(), nonrunningEvents.end(),
						 [](const auto& event)
						 {
							 return std::holds_alternative<InteractionProfileEvent>(event.event);
						 }),
			"Nonrunning sync emitted profile event");
	FrameSchedulerConfig config;
	config.mode = ExecutionMode::Deterministic;
	config.epoch = {Duration{1}};
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session.Ready({Duration{1}}) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Device lifecycle");
	TrackingState input;
	input.head.pose.position.x = 1;
	session.PublishTracking({Duration{1000}}, input);
	const XrActiveActionSet active{set, XR_NULL_PATH};
	const XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
	Require(session.SyncActions(sync) == XR_SUCCESS, "Device sync");
	std::string localized;
	Require(session.Actions().LocalizedName(path("/user/eyes_ext/input/gaze_ext/pose"),
		XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT, localized) == XR_SUCCESS && localized == "Eye Gaze",
		"Gaze localization selected a controller profile");
	Require(session.Actions().LocalizedName(path("/user/vive_tracker_htcx/role/waist/output/haptic"),
		XR_INPUT_SOURCE_LOCALIZED_NAME_USER_PATH_BIT, localized) == XR_SUCCESS &&
		localized == "/user/vive_tracker_htcx/role/waist", "Tracker localization selected a controller profile");
	Require(session.Actions().CurrentProfile(waist, profile) == XR_SUCCESS && profile == suggest.interactionProfile,
			"First synchronization did not select tracker profile");
	const auto synchronizedEvents = instance.Events().Observe();
	const auto profileEvents = std::count_if(synchronizedEvents.begin(), synchronizedEvents.end(),
											 [](const auto& event)
											 {
												 return std::holds_alternative<InteractionProfileEvent>(event.event);
											 });
	Require(profileEvents == 1, "First running synchronization did not notify profile selection");
	XrSpace stage{}, gazeSpace{}, trackerSpace{};
	const XrPosef identity{{0, 0, 0, 1}, {0, 0, 0}};
	Require(session.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, identity, stage) == XR_SUCCESS &&
				session.Spaces().CreateActionSpace(gaze, eyes, identity, gazeSpace) == XR_SUCCESS &&
				session.Spaces().CreateActionSpace(tracker, waist, identity, trackerSpace) == XR_SUCCESS,
			"Device spaces");
	SpaceLocation location;
	Require(session.Spaces().Locate(gazeSpace, stage, {Duration{1000}}, location) == XR_SUCCESS &&
				location.pose.eyeGazeSampleTime == RuntimeTime{Duration{1000}} &&
				location.pose.flags == TrackedPoseFlags,
			"Session gaze source metadata");
	XrHapticActionInfo target{XR_TYPE_HAPTIC_ACTION_INFO, nullptr, vibration, waist};
	XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION, nullptr, XR_INFINITE_DURATION, XR_FREQUENCY_UNSPECIFIED, 0.5f};
	Require(session.ApplyHaptic(&target, reinterpret_cast<XrHapticBaseHeader*>(&pulse)) == XR_SUCCESS,
			"Session tracker haptic");
	auto outputs = session.Haptics().Snapshot({Duration{1000}});
	Require(outputs.size() == 1 && outputs[0].device == InputDeviceId{InputDeviceKind::Tracker, 1},
			"Tracker haptic physical identity");
	input.head.pose.position.x = 5;
	input.gaze.tracked = false;
	input.trackers[0].role = TrackerRole::Camera;
	session.PublishTracking({Duration{2000}}, input);
	Require(session.Spaces().Locate(gazeSpace, stage, {Duration{1000}}, location) == XR_SUCCESS &&
				std::abs(location.pose.pose.position.x - 1) < 0.0001f &&
				location.pose.eyeGazeSampleTime == RuntimeTime{Duration{1000}},
			"Later gaze redirected history");
	Require(session.Spaces().Locate(gazeSpace, stage, {Duration{2000}}, location) == XR_SUCCESS &&
				location.pose.flags == ValidPoseFlags && location.pose.eyeGazeSampleTime == RuntimeTime{Duration{2000}},
			"Untracked gaze validity");
	Require(session.Actions().CurrentProfile(waist, profile) == XR_SUCCESS && profile == suggest.interactionProfile,
			"Role change changed profile before synchronization");
	const auto publishedEvents = instance.Events().Observe();
	Require(std::count_if(publishedEvents.begin(), publishedEvents.end(),
						  [](const auto& event)
						  {
							  return std::holds_alternative<InteractionProfileEvent>(event.event);
						  }) == profileEvents,
			"Role change notified before synchronized profile selection");
	Require(session.SyncActions(sync) == XR_SUCCESS, "Role reassignment sync");
	Require(session.Actions().CurrentProfile(waist, profile) == XR_SUCCESS && profile == XR_NULL_PATH,
			"Role change did not update synchronized profile");
	outputs = session.Haptics().Snapshot({Duration{2000}});
	Require(outputs.size() == 1 && outputs[0].status == HapticStatus::Stopped &&
				outputs[0].end == RuntimeTime{Duration{2000}},
			"Role reassignment retained old output");
	Require(session.Spaces().Locate(trackerSpace, stage, {Duration{1000}}, location) == XR_SUCCESS &&
				!location.pose.flags,
			"Inactive tracker binding remained located");
	const auto beforeRoleCycle = instance.Events().Observe();
	const auto beforeCycleCount = std::count_if(beforeRoleCycle.begin(), beforeRoleCycle.end(),
												[](const auto& event)
												{
													return std::holds_alternative<InteractionProfileEvent>(event.event);
												});
	input.trackers[0].role = TrackerRole::Waist;
	session.PublishTracking({Duration{3000}}, input);
	input.trackers[0].role = TrackerRole::Camera;
	session.PublishTracking({Duration{4000}}, input);
	Require(session.SyncActions(sync) == XR_SUCCESS && session.Actions().CurrentProfile(waist, profile) == XR_SUCCESS &&
				!profile,
			"Inventory role cycle did not preserve final synchronized profile");
	const auto roleCycleEvents = instance.Events().Observe();
	Require(std::count_if(roleCycleEvents.begin(), roleCycleEvents.end(),
						  [](const auto& event)
						  {
							  return std::holds_alternative<InteractionProfileEvent>(event.event);
						  }) == beforeCycleCount + 1,
			"Bound inventory cycle lost deferred profile notification");
	Require(registry.DestroyAction(gaze) == XR_SUCCESS && !registry.Action(gaze),
			"Destroyed action handle remained public");
	XrSpace rejectedSpace{};
	Require(session.Spaces().CreateActionSpace(gaze, eyes, identity, rejectedSpace) == XR_ERROR_HANDLE_INVALID,
			"Destroyed pose action created a new space");
	Require(session.SyncActions(sync) == XR_SUCCESS, "Retired pose action synchronization");
	Require(session.Spaces().Locate(gazeSpace, stage, {Duration{2000}}, location) == XR_SUCCESS &&
				location.pose.flags == ValidPoseFlags && location.pose.eyeGazeSampleTime == RuntimeTime{Duration{2000}},
			"Action destruction invalidated existing action space");
	XrActionStateGetInfo get{XR_TYPE_ACTION_STATE_GET_INFO, nullptr, gaze, eyes};
	XrActionStatePose poseState{XR_TYPE_ACTION_STATE_POSE};
	Require(session.Actions().GetPose(get, poseState) == XR_ERROR_HANDLE_INVALID,
			"Destroyed action remained accessible through state query");
}

void TestRetainedInstanceAndCpuOwnership()
{
	using namespace simxr;
	using namespace std::chrono_literals;
	bool rejected = false;
	try
	{
		Session invalid(reinterpret_cast<XrSession>(30), std::shared_ptr<Instance>{});
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Null shared instance owner accepted");
	const std::array extensions{XR_FB_BODY_TRACKING_EXTENSION_NAME, XR_BD_BODY_TRACKING_EXTENSION_NAME};
	auto instance = std::make_shared<Instance>(reinterpret_cast<XrInstance>(31), extensions, XR_MAKE_VERSION(1, 0, 27));
	std::weak_ptr<Instance> instanceLifetime = instance;
	XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(setInfo.actionSetName, "ownership");
	std::strcpy(setInfo.localizedActionSetName, "Ownership");
	XrActionSet actionSet{};
	Require(instance->Actions().CreateSet(setInfo, actionSet) == XR_SUCCESS, "Owned instance action set");
	XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
	std::strcpy(actionInfo.actionName, "pose");
	std::strcpy(actionInfo.localizedActionName, "Pose");
	actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
	XrAction action{};
	Require(instance->Actions().CreateAction(actionSet, actionInfo, action) == XR_SUCCESS,
			"Owned instance pose action");
	auto session = std::make_unique<Session>(reinterpret_cast<XrSession>(32), instance);
	instance.reset();
	Require(!instanceLifetime.expired(), "Session did not retain shared instance");
	FrameSchedulerConfig configuration;
	configuration.mode = ExecutionMode::Deterministic;
	configuration.epoch = {Duration{1}};
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->Ready({Duration{1}}) == XR_SUCCESS && session->Begin(begin, configuration) == XR_SUCCESS,
			"Retained instance lifecycle");
	XrSpace base{}, actionSpace{};
	const XrPosef identity{{0, 0, 0, 1}, {0, 0, 0}};
	Require(session->Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, identity, base) == XR_SUCCESS &&
				session->Spaces().CreateActionSpace(action, XR_NULL_PATH, identity, actionSpace) == XR_SUCCESS,
			"Retained instance space validation");
	TrackingState input;
	input.head.pose.position.x = 1;
	session->PublishTracking({Duration{1000}}, input);
	auto retained = *session->Tracking().Sample({Duration{1000}});
	input.head.pose.position.x = 3;
	session->PublishTracking({Duration{2000}}, input);
	XrBodyTrackerCreateInfoFB createFB{XR_TYPE_BODY_TRACKER_CREATE_INFO_FB, nullptr, XR_BODY_JOINT_SET_DEFAULT_FB};
	XrBodyTrackerCreateInfoBD createBD{XR_TYPE_BODY_TRACKER_CREATE_INFO_BD, nullptr,
									   XR_BODY_JOINT_SET_BODY_WITHOUT_ARM_BD};
	XrBodyTrackerFB trackerFB{};
	XrBodyTrackerBD trackerBD{};
	Require(session->Body().CreateFB(&createFB, &trackerFB) == XR_SUCCESS &&
				session->Body().CreateBD(&createBD, &trackerBD) == XR_SUCCESS,
			"Session owns body trackers");
	std::array<XrBodyJointLocationFB, XR_BODY_JOINT_COUNT_FB> joints{};
	XrBodyJointLocationsFB locations{XR_TYPE_BODY_JOINT_LOCATIONS_FB, nullptr,		XR_FALSE, 0,
									 XR_BODY_JOINT_COUNT_FB,		  joints.data()};
	XrBodyJointsLocateInfoFB locate{XR_TYPE_BODY_JOINTS_LOCATE_INFO_FB, nullptr, base, 1000};
	Require(session->Body().LocateFB(trackerFB, &locate, &locations) == XR_SUCCESS && locations.isActive,
			"Historical session body query");
	const auto retainedJoints = joints;
	const auto headX = retainedJoints[XR_BODY_JOINT_HEAD_FB].pose.position.x;
	Require(std::abs(headX - 1) < 0.0001f, "Later input redirected historical body query");
	FramePrediction frame;
	Require(session->WaitFrame(frame) == XR_SUCCESS, "Owned session first wait");
	auto pending = std::async(std::launch::async,
							  [&]
							  {
								  FramePrediction next;
								  return session->WaitFrame(next);
							  });
	Eventually(
		[&]
		{
			return session->Scheduler().Snapshot().waitPending;
		},
		"Owned session wait did not block");
	std::atomic<bool> stop = false;
	std::atomic<std::uint32_t> observations = 0;
	auto inspection = std::async(
		std::launch::async,
		[&]
		{
			std::array<XrBodyJointLocationFB, XR_BODY_JOINT_COUNT_FB> threadJoints{};
			XrBodyJointLocationsFB output{
				XR_TYPE_BODY_JOINT_LOCATIONS_FB, nullptr, XR_FALSE, 0, XR_BODY_JOINT_COUNT_FB, threadJoints.data()};
			while (!stop.load())
			{
				auto history = session->Tracking().Sample({Duration{1000}});
				Require(history && std::abs(history->head.pose.position.x - 1) < 0.0001f,
						"Close changed retained tracking history");
				auto bodyResult = session->Body().LocateFB(trackerFB, &locate, &output);
				Require(bodyResult == XR_SUCCESS || bodyResult == XR_ERROR_HANDLE_INVALID,
						"Body query/close ownership race");
				PoseSample pose;
				auto spaceResult = session->Spaces().WorldPose(actionSpace, {Duration{1000}}, pose);
				Require(spaceResult == XR_SUCCESS || spaceResult == XR_ERROR_HANDLE_INVALID,
						"Action space/close ownership race");
				++observations;
			}
		});
	Eventually(
		[&]
		{
			return observations.load() > 0;
		},
		"CPU inspection did not begin");
	session->Close();
	stop.store(true);
	Require(inspection.wait_for(1s) == std::future_status::ready, "CPU inspection did not finish after close");
	inspection.get();
	Require(pending.wait_for(1s) == std::future_status::ready && pending.get() == XR_ERROR_SESSION_NOT_RUNNING,
			"Retained ownership wait cancellation");
	Require(session->Snapshot().closed && !session->Body().OwnsFB(trackerFB) && !session->Body().OwnsBD(trackerBD) &&
				!session->Spaces().Owns(base) && !session->Spaces().Owns(actionSpace),
			"Close retained child CPU handles");
	Require(session->Body().DestroyFB(trackerFB) == XR_ERROR_HANDLE_INVALID &&
				session->Spaces().Destroy(base) == XR_ERROR_HANDLE_INVALID,
			"Closed child handle reuse");
	Require(!instanceLifetime.expired(), "Close released instance before session destruction");
	{
		auto owner = instanceLifetime.lock();
		XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
		Require(owner->Events().Poll(event) == XR_EVENT_UNAVAILABLE && !owner->Events().Observe().empty(),
				"Retained instance event teardown");
	}
	session->Close();
	rejected = false;
	try
	{
		session->PublishTracking({Duration{3000}}, input);
	}
	catch (const std::logic_error&)
	{
		rejected = true;
	}
	Require(rejected, "Closed session accepted tracking publication");
	session.reset();
	Require(instanceLifetime.expired(), "Destroyed session leaked retained instance");
	Require(std::abs(retained.head.pose.position.x - 1) < 0.0001f && retained.body.active &&
				std::abs(retainedJoints[XR_BODY_JOINT_HEAD_FB].pose.position.x - headX) < 0.0001f,
			"Owned CPU results depended on destroyed service storage");
}
} // namespace

int main()
{
	using namespace simxr;
	using namespace std::chrono_literals;
	Instance instance(reinterpret_cast<XrInstance>(1), {}, XR_MAKE_VERSION(1, 0, 27));
	Session session(reinterpret_cast<XrSession>(2), instance);
	FramePrediction prediction;
	Require(session.WaitFrame(prediction) == XR_ERROR_SESSION_NOT_RUNNING, "Session not running");
	FrameSchedulerConfig config;
	config.epoch = {Duration{1}};
	config.mode = ExecutionMode::Deterministic;
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO};
	begin.primaryViewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	Require(session.Begin(begin, config) == XR_ERROR_SESSION_NOT_READY, "Begin before ready");
	Require(session.Ready({Duration{5}}) == XR_SUCCESS, "Initialize ready");
	Require(session.Begin(begin, config) == XR_SUCCESS, "Begin session");
	Require(session.Begin(begin, config) == XR_ERROR_SESSION_RUNNING, "Duplicate begin");
	Require(session.Snapshot().state == XR_SESSION_STATE_FOCUSED, "Initial focus");
	session.SetFocused(false, config.epoch);
	Require(session.Snapshot().state == XR_SESSION_STATE_VISIBLE, "Preview lost focus");
	session.SetFocused(true, config.epoch);
	Require(session.Snapshot().state == XR_SESSION_STATE_FOCUSED, "Preview regained focus");
	XrSpace stage;
	Require(session.Spaces().CreateReferenceSpace(XR_REFERENCE_SPACE_TYPE_STAGE, {{0, 0, 0, 1}, {}}, stage) ==
				XR_SUCCESS,
			"Session reference space");
	TrackingState tracking;
	tracking.head.pose.position.x = 0.3f;
	ViewSettings settings;
	settings.ipd = 0.06f;
	settings.outerWidth = 640;
	session.PublishTracking({Duration{1000}}, tracking, settings);
	tracking.head.pose.position.x = 0.9f;
	settings.ipd = 0.08f;
	settings.outerWidth = 960;
	session.PublishTracking({Duration{2000}}, tracking, settings);
	XrViewLocateInfo locate{XR_TYPE_VIEW_LOCATE_INFO};
	locate.viewConfigurationType = XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO;
	locate.displayTime = 1000;
	locate.space = stage;
	XrViewState viewState{XR_TYPE_VIEW_STATE};
	XrView views[2]{{XR_TYPE_VIEW}, {XR_TYPE_VIEW}};
	std::uint32_t viewCount = 0;
	Require(session.LocateViews(locate, &viewState, 2, &viewCount, views) == XR_SUCCESS && viewCount == 2,
			"Historical session views");
	Require(std::abs(views[0].pose.position.x - 0.27f) < 0.0001f &&
				std::abs(views[1].pose.position.x - 0.33f) < 0.0001f,
			"New settings rewrote historical view configuration");
	const auto compositionViews = session.ViewsAt({Duration{1000}});
	Require(compositionViews.views.size() == 2 && compositionViews.views[0].description.width == 640 &&
				std::abs(compositionViews.views[0].pose.pose.position.x - 0.27f) < 0.0001f,
			"Compositor view target did not use coherent historical tracking and dimensions");
	XrActionSetCreateInfo setInfo{XR_TYPE_ACTION_SET_CREATE_INFO};
	std::strcpy(setInfo.actionSetName, "controls");
	std::strcpy(setInfo.localizedActionSetName, "Controls");
	XrActionSet set;
	auto& registry = instance.Actions();
	Require(registry.CreateSet(setInfo, set) == XR_SUCCESS, "Session action set");
	auto path = [&](const char* name)
	{
		XrPath result;
		Require(registry.StringToPath(name, result) == XR_SUCCESS, "Session action path");
		return result;
	};
	const std::array users{path("/user/hand/left"), path("/user/hand/right")};
	XrActionCreateInfo actionInfo{XR_TYPE_ACTION_CREATE_INFO};
	std::strcpy(actionInfo.actionName, "vibration");
	std::strcpy(actionInfo.localizedActionName, "Vibration");
	actionInfo.actionType = XR_ACTION_TYPE_VIBRATION_OUTPUT;
	actionInfo.countSubactionPaths = 2;
	actionInfo.subactionPaths = users.data();
	XrAction vibration;
	Require(registry.CreateAction(set, actionInfo, vibration) == XR_SUCCESS, "Session output action");
	std::strcpy(actionInfo.actionName, "pose");
	std::strcpy(actionInfo.localizedActionName, "Pose");
	actionInfo.actionType = XR_ACTION_TYPE_POSE_INPUT;
	XrAction pose;
	Require(registry.CreateAction(set, actionInfo, pose) == XR_SUCCESS, "Session pose action");
	const std::array bindings{XrActionSuggestedBinding{vibration, path("/user/hand/left/output/haptic")},
							  XrActionSuggestedBinding{vibration, path("/user/hand/right/output/haptic")},
							  XrActionSuggestedBinding{pose, path("/user/hand/left/input/grip/pose")},
							  XrActionSuggestedBinding{pose, path("/user/hand/right/input/grip/pose")}};
	XrInteractionProfileSuggestedBinding suggest{XR_TYPE_INTERACTION_PROFILE_SUGGESTED_BINDING, nullptr,
												 path("/interaction_profiles/oculus/touch_controller"),
												 static_cast<std::uint32_t>(bindings.size()), bindings.data()};
	Require(registry.SuggestBindings(suggest) == XR_SUCCESS, "Session bindings");
	XrSessionActionSetsAttachInfo attach{XR_TYPE_SESSION_ACTION_SETS_ATTACH_INFO, nullptr, 1, &set};
	Require(session.Attach(attach) == XR_SUCCESS, "Session attachment");
	const XrActiveActionSet active{set, XR_NULL_PATH};
	XrActionsSyncInfo sync{XR_TYPE_ACTIONS_SYNC_INFO, nullptr, 1, &active};
	InputSnapshot input;
	input.time = {Duration{2000}};
	Require(session.Input().Publish(input) == XR_SUCCESS && session.SyncActions(sync) == XR_SUCCESS,
			"Session synchronize");
	XrSpace actionSpace;
	Require(session.Spaces().CreateActionSpace(pose, users[0], {{0, 0, 0, 1}, {}}, actionSpace) == XR_SUCCESS,
			"Session action space");
	SpaceLocation controllerLocation;
	Require(session.Spaces().Locate(actionSpace, stage, {Duration{1000}}, controllerLocation) == XR_SUCCESS &&
				controllerLocation.pose.flags == TrackedPoseFlags,
			"Session action source resolution");
	const std::array devices{HapticDevice::LeftController, HapticDevice::RightController};
	XrHapticVibration pulse{XR_TYPE_HAPTIC_VIBRATION, nullptr, XR_INFINITE_DURATION, XR_FREQUENCY_UNSPECIFIED, 0.5f};
	Require(session.Haptics().Apply(devices, vibration, XR_NULL_PATH, pulse, {Duration{2000}}, true) == XR_SUCCESS,
			"Session haptic output");
	input.time = {Duration{3000}};
	input.controllers[0].connected = false;
	Require(session.Input().Publish(input) == XR_SUCCESS && session.SyncActions(sync) == XR_SUCCESS,
			"Disconnect synchronization");
	auto outputs = session.Haptics().Snapshot({Duration{3000}});
	Require(std::count_if(outputs.begin(), outputs.end(),
						  [](const auto& output)
						  {
							  return output.status == HapticStatus::Active;
						  }) == 1,
			"Disconnected output remained active");
	Require(std::any_of(outputs.begin(), outputs.end(),
						[](const auto& output)
						{
							return output.status == HapticStatus::Active &&
								   output.device == HapticDevice::RightController;
						}),
			"Disconnect stopped the other controller");
	Require(std::any_of(outputs.begin(), outputs.end(),
						[](const auto& output)
						{
							return output.device == HapticDevice::LeftController &&
								   output.end == RuntimeTime{Duration{3000}};
						}),
			"Disconnect output timing used an earlier frame epoch");
	Require(session.Spaces().Locate(actionSpace, stage, {Duration{1000}}, controllerLocation) == XR_SUCCESS &&
				controllerLocation.pose.flags == 0,
			"Inactive pose action located as tracked");
	Require(session.WaitFrame(prediction) == XR_SUCCESS, "Session first frame");
	auto pending = std::async(std::launch::async,
							  [&]
							  {
								  FramePrediction next;
								  return session.WaitFrame(next);
							  });
	Eventually(
		[&]
		{
			return session.Scheduler().Snapshot().waitPending;
		},
		"Wait did not enter");
	Require(pending.wait_for(20ms) == std::future_status::timeout, "Admission passed begin gate");
	Require(session.BeginFrame() == XR_SUCCESS, "Begin frame");
	Require(pending.wait_for(1s) == std::future_status::ready && pending.get() == XR_SUCCESS, "Overlapping frame wait");
	const auto generation = session.Snapshot().generation;
	const auto issuedRecord = session.History().Find({generation, {1}});
	Require(issuedRecord && issuedRecord->stage == FrameStage::Begun, "Session begin was not recorded");
	XrFrameEndInfo submittedInfo{XR_TYPE_FRAME_END_INFO, nullptr, 1000, XR_ENVIRONMENT_BLEND_MODE_OPAQUE, 0, nullptr};
	Require(session.EndFrame(prediction, &submittedInfo) == XR_SUCCESS && prediction.id == FrameId{1},
			"Wrong submitted prediction");
	const auto submittedRecord = session.History().Find({generation, {1}});
	Require(submittedRecord && submittedRecord->stage == FrameStage::Submitted && submittedRecord->submission &&
				submittedRecord->submission->layerCount == 0 &&
				submittedRecord->submission->displayTime == RuntimeTime{Duration{1000}} &&
				std::abs(submittedRecord->submission->viewSettings.ipd - 0.06f) < 0.0001f &&
				std::abs(submittedRecord->submission->tracking->head.pose.position.x - 0.3f) < 0.0001f &&
				issuedRecord->stage == FrameStage::Begun,
			"Session submission did not retain exact CPU state");
	Require(session.Snapshot().lastSubmitted == FrameId{1} && session.Snapshot().lastIssued.id == FrameId{2},
			"Pipeline identities mixed");
	Require(session.End({Duration{20}}) == XR_ERROR_SESSION_NOT_STOPPING, "End without request");
	Require(session.RequestExit({Duration{20}}) == XR_SUCCESS, "Request exit");
	session.SetFocused(false, {Duration{20}});
	session.SetFocused(true, {Duration{20}});
	Require(session.Snapshot().state == XR_SESSION_STATE_STOPPING, "Preview focus cancelled stop request");
	Require(session.End({Duration{20}}) == XR_SUCCESS, "End session");
	Require(session.Ready({Duration{30}}) == XR_SUCCESS && session.Begin(begin, config) == XR_SUCCESS,
			"Restart session");
	Require(session.Snapshot().generation != generation, "Session restart reused history identity");
	Require(bool(session.PreviewHistory().Reserve({session.Snapshot().generation, {1}})),
			"Session restart closed preview admission");
	Require(session.History().Find({generation, {2}})->stage == FrameStage::Cancelled,
			"Session end fabricated completion");
	Require(session.WaitFrame(prediction) == XR_SUCCESS && prediction.id == FrameId{1}, "Restart scheduling");
	pending = std::async(std::launch::async,
						 [&]
						 {
							 FramePrediction next;
							 return session.WaitFrame(next);
						 });
	Eventually(
		[&]
		{
			return session.Scheduler().Snapshot().waitPending;
		},
		"Closing wait did not enter");
	session.Close();
	Require(!session.PreviewHistory().Reserve({session.Snapshot().generation, {2}}),
			"Closed session accepted preview admission");
	Require(!session.Spaces().Owns(stage), "Closed session retained spaces");
	Require(pending.wait_for(1s) == std::future_status::ready && pending.get() == XR_ERROR_SESSION_NOT_RUNNING,
			"Close did not cancel wait");
	XrEventDataBuffer event{XR_TYPE_EVENT_DATA_BUFFER};
	Require(instance.Events().Poll(event) == XR_EVENT_UNAVAILABLE, "Closed session left queued handles");
	Require(!instance.Events().Observe().empty(), "Teardown consumed observation history");
	Require(session.Ready({Duration{40}}) == XR_ERROR_SESSION_LOST, "Closed session reactivated");
	TestPresentationPolicyLifecycle();
	TestConcurrentNativeControlSynchronization();
	TestRetainedInstanceAndCpuOwnership();
	TestInteractionDevices();
	TestCaptureConfigurationOwnership();
	TestCaptureReservations();
	TestCaptureBoundary();
	TestRejectedOutputDestruction();
	TestReentrantCaptureCollection();
	TestCaptureProducerRetry();
	TestSequenceCaptureAdmission();
	TestSequenceCaptureUnsupportedAndOwnership();
	TestSequenceStopReleasesHeldInput();
	TestLayerViewCapture();
	TestLayerCaptureRejectionReentry();
	TestLiveSubmissionPolicy();
	TestQuadHistoryCaptureCapacity();
	TestCaptureBudgetPromotion();
	TestLateCaptureBudgetPromotion();
	TestQuadHistoryWithoutSnapshotCapacity();
	std::cout << "Session tests passed\n";
}
