#include "../TestSupport.h"
#include "Pch.h"
#include "control/ControlPacket.h"
#include "control/ControlRequestError.h"
#include "control/ControlSchema.h"
#include "control/ControlSessions.h"
#include "control/LiveInputControl.h"
#include "control/SimulationConnection.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include "runtime/Session.h"
#include "simulation/BodyModel.h"
#include "simulation/GameBridge.h"

#include <future>
#include <iostream>

namespace simxr::tests
{
using namespace control;

static void BodyCodec()
{
	for (const auto set :
		 {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA, BodyJointSet::WithoutArmsBD, BodyJointSet::FullBodyBD})
	{
		LiveInputSample sample;
		sample.targetTime = {Duration{9007199254740993LL}};
		sample.body.emplace();
		auto& body = *sample.body;
		body.jointSet = set;
		body.requestedTime = sample.targetTime;
		for (const auto joint : BodyModel::JointMap(set))
			body.joints[static_cast<std::size_t>(joint)] = ObservedBodyJoint{IdentityPose(), 0};
		if (set == BodyJointSet::FullBodyMETA)
		{
			body.located = true;
			body.active = true;
			body.confidence = 1;
			body.returnedTime = body.requestedTime;
			body.skeletonRevision = 1;
			body.skeleton.emplace();
			for (const auto joint : BodyModel::JointMap(set))
				(*body.skeleton)[static_cast<std::size_t>(joint)] = SkeletonJoint{joint, {}, IdentityPose()};
		}
		for (unsigned mask = 0; mask < 4; ++mask)
		{
			sample.eyeViews = mask & 1 ? std::optional{StereoEyeViews{}} : std::nullopt;
			sample.trackers = mask & 2 ? std::optional{std::vector<TrackerDevice>{}} : std::nullopt;
			if (sample.trackers)
				for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
				{
					TrackerDevice tracker{{index + 1}, TrackerRole::Unassigned};
					tracker.sourcePath = TrackerSourcePath{"htcx:/" + std::string(275, 'a') + std::to_string(index)};
					tracker.poseOverride = IdentityPose();
					sample.trackers->push_back(tracker);
				}
			Message request;
			request.fields = EncodeLiveInput(sample);
			const auto decoded = DecodeLiveInput(request);
			Require(decoded.body && decoded.body->jointSet == set && decoded.body->requestedTime == sample.targetTime &&
						decoded.eyeViews.has_value() == sample.eyeViews.has_value() &&
						decoded.trackers.has_value() == sample.trackers.has_value(),
					"Live body codec lost schema, exact time or optional-component layout");
			Require(EncodeFields(EncodeLiveInput(decoded)) == EncodeFields(request.fields),
					"Live body codec is not canonical");
		}
	}
}

static void TrackerCodec()
{
	LiveInputSample sample;
	sample.trackers.emplace();
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
	{
		TrackerDevice tracker{{100 + index}, TrackerRole::Unassigned};
		tracker.poseOverride = XrPosef{{0, 0, 0, 1}, {1, 2, 3}};
		tracker.poseFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT;
		sample.trackers->push_back(tracker);
	}
	for (bool eyes : {false, true})
	{
		if (eyes)
			sample.eyeViews.emplace();
		Message message;
		message.fields = EncodeLiveInput(sample);
		const auto decoded = DecodeLiveInput(message);
		Require(decoded.eyeViews.has_value() == eyes && decoded.trackers && decoded.trackers->size() == 64 &&
					decoded.trackers->back().identity.value == 163 &&
					decoded.trackers->back().poseFlags == XR_SPACE_LOCATION_POSITION_VALID_BIT,
				"Optional eye/tracker components changed during decoding");
	}
	const auto rejectComponents = [&](std::vector<Field> components) {
		Message malformed;
		malformed.fields = EncodeLiveInput(sample);
		std::vector<std::byte> encoded;
		for (const auto& component : components)
		{
			const auto bytes = EncodeFields(std::span{&component, 1});
			encoded.insert(encoded.end(), bytes.begin(), bytes.end());
		}
		for (auto& field : malformed.fields)
			if (field.tag == tag::Commands)
				field = Field::Bytes(tag::Commands, encoded, true);
		bool rejected = false;
		try
		{
			DecodeLiveInput(malformed);
		}
		catch (const ControlRequestError&)
		{
			rejected = true;
		}
		Require(rejected, "Malformed optional live components were accepted");
	};
	Message complete;
	complete.fields = EncodeLiveInput(sample);
	const auto components = DecodeFields(complete.Find(tag::Commands)->value).fields;
	auto malformed = components;
	std::swap(malformed[5], malformed[6]);
	rejectComponents(malformed);
	malformed = components;
	malformed[6] = malformed[5];
	rejectComponents(malformed);
	malformed = components;
	malformed[6] = EncodeSimulationCommand(SetEyeViews{sample.eyeViews}, 7);
	rejectComponents(malformed);
	sample.trackers->clear();
	Message empty;
	empty.fields = EncodeLiveInput(sample);
	Require(DecodeLiveInput(empty).trackers->empty(), "Explicit empty inventory was lost");
	sample.trackers.reset();
	empty.fields = EncodeLiveInput(sample);
	Require(!DecodeLiveInput(empty).trackers, "Absent inventory became an owned empty inventory");
}

static void Transport()
{
	Instance instance(reinterpret_cast<XrInstance>(991), {}, XR_MAKE_VERSION(1, 0, 27));
	auto session = std::make_shared<Session>(reinterpret_cast<XrSession>(992), instance);
	FrameSchedulerConfig timing;
	timing.mode = ExecutionMode::Deterministic;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS, "Start session");
	ControlSession binding{{1}, reinterpret_cast<XrSession>(992), session};
	GameBridge game;
	std::vector<SnapshotRecord> held;
	auto connection = std::make_unique<SimulationConnection>(ConnectionId{23}, game);
	std::uint64_t requestId = 0;
	const auto send = [&](OpCode opcode, std::vector<Field> fields = {}, std::uint16_t minor = ProtocolMinor) {
		Message request;
		request.opcode = static_cast<std::uint16_t>(opcode);
		request.requestId = {++requestId};
		request.fields = std::move(fields);
		request.fields.push_back(Field::U64(tag::Session, 1, true));
		request.fields.push_back(Field::U64(tag::Generation, session->Snapshot().generation.value, true));
		const auto decoded = Decode(Encode(request));
		Require(decoded.message.has_value(), "Roundtrip request framing");
		ValidateControlRequest(ControlPacket{*decoded.message}, false);
		Message response;
		response.minor = minor;
		return connection->Dispatch(*decoded.message, binding, response, held);
	};
	const auto claim = [&](std::uint64_t transaction, std::uint64_t frame) {
		const std::array commands{EncodeSimulationCommand(TakeControl{ControlChannel::Head}, 1),
								  EncodeSimulationCommand(TakeControl{ControlChannel::LeftPose}, 2),
								  EncodeSimulationCommand(TakeControl{ControlChannel::LeftControls}, 3)};
		return send(OpCode::SubmitSimulation,
					{Field::U64(tag::TransactionId, transaction), Field::U32(tag::SourceKind, 4),
					 Field::U32(tag::LatePolicy, 0), Field::U64(tag::ApplyFrame, frame),
					 Field::Bytes(tag::Commands, EncodeFields(commands))});
	};
	bool rejected = false;
	try
	{
		claim(1, 1);
	}
	catch (const ControlRequestError& error)
	{
		rejected = error.error == ErrorCode::Conflict;
	}
	Require(rejected, "Ordinary connection cannot claim device source kind");
	const auto attached = send(OpCode::AttachLiveInput);
	const auto attachment = *attached.Find(tag::AttachmentId)->AsU64();
	Require(!DecodeLiveClock(send(OpCode::ReadLiveClock, {Field::U64(tag::AttachmentId, attachment)})),
			"Deterministic session cannot advertise a wall-clock mapping");
	Require(claim(1, 1).Find(tag::Phase)->AsText() == "queued", "Queue attached channel claim");
	session->Commands().Apply({1}, {}, timing.epoch, {});
	LiveInputSample sample;
	sample.connection = attachment;
	sample.sequence = 1;
	sample.observedTime = {Duration{10}};
	sample.targetTime = {Duration{20}};
	sample.body.emplace();
	sample.body->requestedTime = sample.targetTime;
	for (const auto joint : BodyModel::JointMap(sample.body->jointSet))
		sample.body->joints[static_cast<std::size_t>(joint)] = ObservedBodyJoint{IdentityPose(), 0};
	bool oldBodyRejected = false;
	try
	{
		send(OpCode::SubmitLiveInput, EncodeLiveInput(sample), 12);
	}
	catch (const ControlRequestError& error)
	{
		oldBodyRejected = error.error == ErrorCode::UnsupportedField;
	}
	Require(oldBodyRejected, "Old connection silently accepted live body input");
	sample.body.reset();

	sample.maxAge = std::chrono::milliseconds(250);
	sample.head.flags = TrackedPoseFlags;
	sample.hands[0].aim = PoseSample{};
	sample.hands[0].aim->pose.position.x = 7;
	sample.controls[0].primary = true;
	Message suspended;
	sample.controlsActive = false;
	suspended.fields = EncodeLiveInput(sample);
	Require(!DecodeLiveInput(suspended).controlsActive, "Transport preserves focus suspension");
	std::erase_if(suspended.fields, [](const auto& field) { return field.tag == tag::Enabled; });
	Require(DecodeLiveInput(suspended).controlsActive, "Legacy live samples default to active controls");
	sample.controlsActive = true;
	Message malformed;
	malformed.fields = EncodeLiveInput(sample);
	for (auto& field : malformed.fields)
		if (field.tag == tag::Commands)
		{
			auto components = DecodeFields(field.value).fields;
			components[1] = EncodeSimulationCommand(SetControllerPose{1, sample.hands[1]}, 2);
			field = Field::Bytes(tag::Commands, EncodeFields(components));
		}
	rejected = false;
	try
	{
		DecodeLiveInput(malformed);
	}
	catch (const ControlRequestError& error)
	{
		rejected = error.error == ErrorCode::Invalid;
	}
	Require(rejected, "Reject duplicate or swapped hand components");
	Message eyeMessage;
	auto eyeSample = sample;
	eyeSample.eyeViews.emplace();
	eyeSample.eyeViews->spaceRevision = 8;
	eyeSample.eyeViews->poses[0].flags = TrackedPoseFlags;
	eyeMessage.fields = EncodeLiveInput(eyeSample);
	Require(DecodeLiveInput(eyeMessage).eyeViews->spaceRevision == 8, "Decode optional physical eye component");
	for (auto& field : eyeMessage.fields)
		if (field.tag == tag::Commands)
		{
			auto components = DecodeFields(field.value).fields;
			components[5] = EncodeSimulationCommand(SetControllerControls{2, {}}, 6);
			field = Field::Bytes(tag::Commands, EncodeFields(components));
		}
	rejected = false;
	try
	{
		DecodeLiveInput(eyeMessage);
	}
	catch (const ControlRequestError& error)
	{
		rejected = error.error == ErrorCode::Invalid;
	}
	Require(rejected, "Sixth component cannot address an out-of-range controller");
	auto invalid = sample;
	invalid.targetTime = {Duration{9}};
	Require(send(OpCode::SubmitLiveInput, EncodeLiveInput(invalid)).Find(tag::Phase)->AsText() == "invalid",
			"Reject prediction before observation without consuming sample identity");
	Require(send(OpCode::SubmitLiveInput, EncodeLiveInput(sample)).Find(tag::Phase)->AsText() == "queued",
			"Admit native sample");
	const auto applied = session->Commands().Apply({2}, {}, timing.epoch, {});
	Require(applied->configuration.input.controllers[0].primary &&
				applied->configuration.tracking.controllers[0].aim->pose.position.x == 7,
			"Preserve independent aim and controls over wire");
	session->PublishInput({});
	Require(session->Input().Snapshot().controllers[0].primary, "Publish physical controls");
	send(OpCode::DetachLiveInput, {Field::U64(tag::AttachmentId, attachment)});
	Require(!session->Input().Snapshot().controllers[0].primary, "Detach releases held controls immediately");
	const auto reattached = send(OpCode::AttachLiveInput);
	Require(*reattached.Find(tag::AttachmentId)->AsU64() > attachment, "Reconnect changes attachment identity");
	rejected = false;
	try
	{
		send(OpCode::SubmitLiveInput, EncodeLiveInput(sample));
	}
	catch (const ControlRequestError& error)
	{
		rejected = error.error == ErrorCode::Conflict;
	}
	Require(rejected, "Reject delayed old attachment sample");
	claim(2, 3);
	session->Commands().Apply({3}, {}, timing.epoch, {});
	sample.connection = *reattached.Find(tag::AttachmentId)->AsU64();
	send(OpCode::SubmitLiveInput, EncodeLiveInput(sample));
	session->Commands().Apply({4}, {}, timing.epoch, {});
	session->PublishInput({});
	connection.reset();
	Require(!session->Input().Snapshot().controllers[0].primary && !session->Commands().LiveInputState().owner,
			"Connection destruction releases attachment without a game frame");
	connection = std::make_unique<SimulationConnection>(ConnectionId{24}, game);
	Require(session->RequestExit(timing.epoch) == XR_SUCCESS && session->End(timing.epoch) == XR_SUCCESS,
			"End deterministic session before real-time clock test");
	timing.mode = ExecutionMode::RealTime;
	Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS,
			"Begin real-time clock session");
	const auto realAttachment = *send(OpCode::AttachLiveInput).Find(tag::AttachmentId)->AsU64();
	const auto readClock = [&] {
		return send(OpCode::ReadLiveClock, {Field::U64(tag::AttachmentId, realAttachment)});
	};
	const auto wireClock = readClock();
	const auto mapped = DecodeLiveClock(wireClock);
	Require(mapped && mapped->generation == session->Snapshot().generation && mapped->revision &&
				mapped->runtimeBefore <= mapped->runtimeAfter && mapped->nativeNanoseconds > 0,
			"Wire clock preserves native domain, scheduler revision and measured interval");
	auto corrupt = wireClock;
	for (auto& field : corrupt.fields)
		if (field.tag == tag::ClockDomain)
			field = Field::U32(tag::ClockDomain, 99);
	rejected = false;
	try
	{
		DecodeLiveClock(corrupt);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Host decoder rejects an unknown native clock domain");
	claim(3, 1);
	session->Commands().Apply({1}, {}, timing.epoch, {});
	sample.connection = realAttachment;
	sample.sequence = 1;
	sample.observedTime = mapped->runtimeBefore;
	sample.targetTime = {mapped->runtimeAfter.value + std::chrono::milliseconds(100)};
	sample.clockRevision = 0;
	Require(send(OpCode::SubmitLiveInput, EncodeLiveInput(sample)).Find(tag::Phase)->AsText() == "conflict",
			"Real-time input requires an explicit current clock revision");
	sample.clockRevision = mapped->revision;
	Require(send(OpCode::SubmitLiveInput, EncodeLiveInput(sample)).Find(tag::Phase)->AsText() == "queued",
			"Current clock revision admits input");
	const auto predict = [&](std::uint64_t sequence) {
		return send(OpCode::PublishDisplayPrediction,
					{Field::U64(tag::AttachmentId, realAttachment), Field::U64(tag::SampleSequence, sequence),
					 Field::U64(tag::ClockRevision, sample.clockRevision),
					 Field::I64(tag::PredictionTime, sample.targetTime.value.count()),
					 Field::I64(tag::Period, 50'000'000)});
	};
	Require(predict(2).Find(tag::Phase)->AsText() == "conflict",
			"Prediction must correlate to the accepted input sequence");
	Require(predict(1).Find(tag::Phase)->AsText() == "applied", "Attachment drives both pacing components");
	Require(predict(1).Find(tag::Phase)->AsText() == "conflict", "Duplicate display prediction is not admitted twice");
	FramePrediction physical;
	Require(session->Scheduler().WaitFrame(physical) == XR_SUCCESS && physical.displayTime == sample.targetTime &&
				physical.displayPeriod == std::chrono::milliseconds(50),
			"Wire prediction reaches application admission unchanged");
	Eventually(
		[&] {
			const auto selected = session->Display().Snapshot().latest;
			return selected && selected->time == sample.targetTime;
		},
		"Wire prediction reaches physical display opportunity");
	session->Commands().Apply({2}, {}, timing.epoch, {}, {}, LiveInputClock::now(), mapped->revision);
	session->PublishInput({});
	Require(session->Input().Snapshot().controllers[0].primary, "Publish live control before clock pause");
	session->Scheduler().Pause();
	Require(!DecodeLiveClock(readClock()), "Paused attachment has no live clock mapping");
	Require(predict(1).Find(tag::Phase)->AsText() == "conflict", "Paused session rejects physical pacing");
	Require(send(OpCode::SubmitLiveInput, EncodeLiveInput(sample)).Find(tag::Phase)->AsText() == "conflict",
			"Paused clock cannot admit a mapped sample");
	const auto deadline = LiveInputClock::now() + std::chrono::seconds(3);
	while (session->Input().Snapshot().controllers[0].primary && LiveInputClock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	Require(!session->Input().Snapshot().controllers[0].primary && !session->Commands().LiveInputState().owner,
			"Clock invalidation releases held input without another frame");
	Require(session->Scheduler().Resume(session->ClockSource()->Now(), {1}) == XR_SUCCESS, "Resume mapping");
	Require(DecodeLiveClock(readClock())->revision != mapped->revision, "Transport observes clock rebase revision");
	Require(send(OpCode::SubmitLiveInput, EncodeLiveInput(sample)).Find(tag::Phase)->AsText() == "conflict",
			"Resume rejects the previous clock revision");
	send(OpCode::DetachLiveInput, {Field::U64(tag::AttachmentId, realAttachment)});
	const auto recovered = *send(OpCode::AttachLiveInput).Find(tag::AttachmentId)->AsU64();
	const auto recoveredClock =
		DecodeLiveClock(send(OpCode::ReadLiveClock, {Field::U64(tag::AttachmentId, recovered)}));
	Require(recoveredClock.has_value(), "Recovery obtains a fresh clock mapping");
	claim(4, 3);
	session->Commands().Apply({3}, {}, timing.epoch, {});
	sample.connection = recovered;
	sample.sequence = 1;
	sample.clockRevision = recoveredClock->revision;
	sample.observedTime = recoveredClock->runtimeBefore;
	sample.targetTime = {recoveredClock->runtimeAfter.value + std::chrono::milliseconds(20)};
	Require(send(OpCode::SubmitLiveInput, EncodeLiveInput(sample)).Find(tag::Phase)->AsText() == "queued",
			"Recovered source can publish fresh input");
	Require(
		send(OpCode::PublishDisplayPrediction,
			 {Field::U64(tag::AttachmentId, recovered), Field::U64(tag::SampleSequence, sample.sequence),
			  Field::U64(tag::ClockRevision, sample.clockRevision),
			  Field::I64(tag::PredictionTime, sample.targetTime.value.count()), Field::I64(tag::Period, 50'000'000)})
				.Find(tag::Phase)
				->AsText() == "applied",
		"Recovered source attaches coordinated timing");
	FramePrediction retired;
	Require(session->Scheduler().BeginFrame() == XR_SUCCESS && session->Scheduler().EndFrame(retired) == XR_SUCCESS &&
				retired.displayTime == physical.displayTime,
			"Pause/recovery preserves previously issued physical frame");
	connection.reset();
	FramePrediction fallback;
	auto waiting = std::async(std::launch::async, [&] { return session->Scheduler().WaitFrame(fallback); });
	const auto cleanup = std::shared_ptr<void>(nullptr, [&](void*) { session->Scheduler().Stop(); });
	Require(waiting.wait_for(std::chrono::seconds(1)) == std::future_status::ready && waiting.get() == XR_SUCCESS &&
				fallback.displayPeriod == timing.period && fallback.displayTime > physical.displayTime,
			"Connection destruction restores virtual pacing without waiting for another host pulse");
	Require(!session->Commands().LiveInputState().owner, "Timing disconnect also releases input ownership");
	session->Close();
}
} // namespace simxr::tests

int main()
{
	try
	{
		simxr::tests::BodyCodec();
		simxr::tests::TrackerCodec();
		simxr::tests::Transport();
		std::cout << "Live input transport checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
	catch (const simxr::control::ControlRequestError& error)
	{
		std::cerr << error.detail << '\n';
		return 1;
	}
}
