#include "../TestSupport.h"
#include "Pch.h"
#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "headset/HeadsetInputBridge.h"
#include "math/Math.h"
#include "runtime/Runtime.h"
#include "runtime/SimulationExtensions.h"
#include "simulation/BodyModel.h"

#include <iostream>
#include <unistd.h>

namespace simxr::tests
{
static void NeutralRestart(std::optional<BodyJointSet> bodySet = {}, bool forwardBody = true, bool omitBody = false)
{
	ExtensionRegistry extensions{SimulationExtensions()};
	Runtime runtime{extensions};
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
	create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	std::strcpy(create.applicationInfo.applicationName, "Neutral restart test");
	Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
				runtime.CreateSession(instance, session) == XR_SUCCESS,
			"Create neutral restart session");
	FrameSchedulerConfig config;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->Ready(config.epoch) == XR_SUCCESS && session->Begin(begin, config) == XR_SUCCESS,
			"Start neutral restart session");
	control::ControlService service(runtime);
	const auto endpoint =
		(std::filesystem::temp_directory_path() / ("simxr-headset-neutral-" + std::to_string(getpid()) + ".sock"))
			.string();
	control::LocalControlServer server(service, endpoint);
	headset::HeadsetInputTarget target{endpoint, 1, session->Snapshot().generation, IdentityPose(), 2};
	target.requireNeutral = true;
	target.body = bodySet.has_value() && forwardBody;
	headset::HeadsetInputBridge bridge(target);
	std::uint64_t frame = 0;
	const auto deadline = LiveInputClock::now() + std::chrono::seconds(3);
	while (bridge.Status().phase == headset::HeadsetInputPhase::Connecting && LiveInputClock::now() < deadline)
	{
		session->Commands().Apply({++frame}, {}, config.epoch, {});
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	}
	Require(bridge.Status().phase == headset::HeadsetInputPhase::Ready, bridge.Status().error.c_str());
	session->Commands().StartRecording({}, {});
	headset::HeadsetInputSample input;
	input.head.pose = IdentityPose();
	input.head.locationFlags = ValidPoseFlags;
	input.focused = input.controlsReady = true;
	if (bodySet && !omitBody)
	{
		auto& body = input.body.emplace();
		body.jointSet = *bodySet;
		body.requestedTime = input.time = 100;
		body.located = true;
		if (*bodySet == BodyJointSet::UpperBodyFB || *bodySet == BodyJointSet::FullBodyMETA)
		{
			body.returnedTime = 90;
			body.active = true;
			body.confidence = .75f;
			body.skeletonRevision = 0;
			body.skeleton.emplace();
		}
		else
			body.allJointPosesTracked = false;
		for (const auto joint : BodyModel::JointMap(*bodySet))
		{
			const auto index = static_cast<std::size_t>(joint);
			body.joints[index] = headset::PhysicalBodyJoint{IdentityPose(), XR_SPACE_LOCATION_ORIENTATION_VALID_BIT};
			if (body.skeleton)
				(*body.skeleton)[index] = SkeletonJoint{joint, {}, IdentityPose()};
		}
		if (!forwardBody)
			body.requestedTime = -1;
	}
	input.hands[0].controls.primary = true;
	const auto publish = [&] {
		const auto now = ReadNativeClockSample();
		Require(bridge.Publish(input, now.domain, now.nanoseconds, now.nanoseconds + 10000000,
							   input.body && input.body->returnedTime
								   ? std::optional<std::int64_t>{now.nanoseconds + 9000000}
								   : std::nullopt),
				"Queue restart observation");
	};
	for (int index = 0; index < 10; ++index)
	{
		publish();
		std::this_thread::sleep_for(std::chrono::milliseconds(2));
	}
	Require(bridge.Status().sent == 0, "Recreated host forwarded held controls before neutral recovery");
	input.hands[0].controls.primary = false;
	publish();
	while (!bridge.Status().sent && bridge.Status().phase != headset::HeadsetInputPhase::Failed &&
		   LiveInputClock::now() < deadline)
		std::this_thread::sleep_for(std::chrono::milliseconds(1));
	if (omitBody)
		Require(bridge.Status().phase == headset::HeadsetInputPhase::Failed &&
					bridge.Status().error.find("body provider omitted") != std::string::npos,
				"Missing selected body provider must fail explicitly before submission");
	else
	{
		Require(bridge.Status().sent == 1, bridge.Status().error.c_str());
		session->Commands().Apply({++frame}, {}, config.epoch, {}, {}, LiveInputClock::now(),
								  *session->Scheduler().LiveInputClockRevision());
		const auto applied = session->Commands().Snapshot().applied;
		const auto& body = applied->configuration.tracking.bodyObservation;
		Require(body.has_value() == (bodySet.has_value() && forwardBody),
				"Body forwarding did not match selected ownership across production transport");
		const auto recording = session->Commands().Recorder().Snapshot();
		bool recordedBody = false;
		for (const auto& outcome : recording.transactions)
			for (const auto& command : outcome.transaction.commands)
				if (const auto value = std::get_if<SetBodyControls>(&command); value && value->observation)
				{
					Require(body && outcome.appliedRuntimeTime && outcome.liveInput &&
								value->observation->requestedTime == body->requestedTime &&
								value->observation->returnedTime == body->returnedTime &&
								value->observation->jointSet == body->jointSet,
							"Recording lost applied body time, provider or live-source correlation");
					recordedBody = true;
				}
		Require(recordedBody == body.has_value(), "Recording body presence differs from applied input");
		if (body)
		{
			Require(body->jointSet == *bodySet && body->located &&
						body->joints[static_cast<std::size_t>(BodyModel::JointMap(*bodySet).front())]->flags ==
							XR_SPACE_LOCATION_ORIENTATION_VALID_BIT,
					"Live body publication changed provider schema or partial validity");
			if (body->returnedTime)
				Require(body->requestedTime.value - body->returnedTime->value == Duration{1000000},
						"Transport changed the independently converted body sample age");
			const auto tracked = *input.body;
			const auto bodyOwner = applied->owners[static_cast<std::size_t>(ControlChannel::Body)];
			const auto publishBoundary = [&] {
				const auto expected = bridge.Status().sent + 1;
				publish();
				while (bridge.Status().sent < expected && bridge.Status().phase == headset::HeadsetInputPhase::Ready &&
					   LiveInputClock::now() < deadline)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				Require(bridge.Status().sent == expected, bridge.Status().error.c_str());
				session->Commands().Apply({++frame}, {}, config.epoch, {}, {}, LiveInputClock::now(),
										  *session->Scheduler().LiveInputClockRevision());
				return session->Commands().Snapshot().applied;
			};
			input.focused = false;
			input.controlsReady = false;
			input.body = headset::PhysicalBodySample{};
			input.body->jointSet = *bodySet;
			input.body->requestedTime = input.time;
			for (const auto joint : BodyModel::JointMap(*bodySet))
				input.body->joints[static_cast<std::size_t>(joint)] = headset::PhysicalBodyJoint{};
			const auto unfocused = publishBoundary();
			const auto& unavailable = unfocused->configuration.tracking.bodyObservation;
			Require(unfocused->owners[static_cast<std::size_t>(ControlChannel::Body)] == bodyOwner && unavailable &&
						!unavailable->located && !unavailable->active && !unavailable->skeleton &&
						std::all_of(unavailable->joints.begin(), unavailable->joints.end(),
									[](const auto& joint) { return !joint || !joint->flags; }),
					"Focus loss fabricated a body fallback or retained previously valid joints");
			input.focused = input.controlsReady = true;
			input.body = tracked;
			if (input.body->skeletonRevision)
				++*input.body->skeletonRevision;
			const auto recovered = publishBoundary();
			const auto& resumed = recovered->configuration.tracking.bodyObservation;
			Require(recovered->owners[static_cast<std::size_t>(ControlChannel::Body)] == bodyOwner && resumed &&
						resumed->located && resumed->skeletonRevision == input.body->skeletonRevision &&
						resumed->joints[static_cast<std::size_t>(BodyModel::JointMap(*bodySet).front())]->flags ==
							XR_SPACE_LOCATION_ORIENTATION_VALID_BIT,
					"Focus recovery lost body ownership, partial validity or skeleton revision");
		}
	}

	bridge.Stop();
	session->Commands().Apply({++frame}, {}, config.epoch, {});
	const auto released = session->Commands().Snapshot().applied;
	Require(!released->owners[static_cast<std::size_t>(ControlChannel::Body)] &&
				!released->configuration.tracking.bodyObservation,
			"Stopping physical input must release Body and restore its authored baseline");
	server.Close();
	session->Close();
}

static void Forwarding()
{
	ExtensionRegistry extensions{SimulationExtensions()};
	Runtime runtime{extensions};
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
	create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 0);
	std::strcpy(create.applicationInfo.applicationName, "Native bridge tests");
	Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
				runtime.CreateSession(instance, session) == XR_SUCCESS,
			"Create native transport session");
	FrameSchedulerConfig config;
	const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->Ready(config.epoch) == XR_SUCCESS && session->Begin(begin, config) == XR_SUCCESS,
			"Start real-time session");
	control::ControlService service(runtime);
	const auto endpoint =
		(std::filesystem::temp_directory_path() / ("simxr-headset-input-" + std::to_string(getpid()) + ".sock"))
			.string();
	control::LocalControlServer server(service, endpoint);
	std::uint64_t frame = 0;
	const auto tick = [&] {
		++frame;
		session->Commands().Apply({frame}, {Duration{static_cast<std::int64_t>(frame) * 1000000}},
								  {config.epoch.value + Duration{static_cast<std::int64_t>(frame) * 1000000}}, {}, {},
								  LiveInputClock::now(), *session->Scheduler().LiveInputClockRevision());
		session->PublishInput({});
	};
	const CommandSource existing{CommandSourceKind::Rig, 900};
	CommandTransaction claim;
	claim.id = 1;
	claim.source = existing;
	claim.target.frame = FrameId{frame + 1};
	claim.commands = {TakeControl{ControlChannel::Head}, TakeControl{ControlChannel::Body}};
	Require(session->Commands().Submit(claim).result == CommandResult::Queued, "Queue foreign owner fixture");
	tick();
	for (const bool takeOver : {false, true})
	{
		headset::HeadsetInputTarget target{endpoint, 1, session->Snapshot().generation, IdentityPose(), 1};
		target.takeOver = takeOver;
		headset::HeadsetInputBridge bridge(target);
		const auto deadline = LiveInputClock::now() + std::chrono::seconds(3);
		while (bridge.Status().phase == headset::HeadsetInputPhase::Connecting && LiveInputClock::now() < deadline)
		{
			tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		const auto owners = session->Commands().Snapshot().applied->owners;
		Require(owners[static_cast<std::size_t>(ControlChannel::Body)] == existing,
				"Native takeover must not replace unrelated body ownership");
		if (takeOver)
		{
			Require(bridge.Status().phase == headset::HeadsetInputPhase::Ready,
					"Explicit takeover must acquire the headset channels over production transport");
			for (const auto channel :
				 {ControlChannel::Head, ControlChannel::Views, ControlChannel::LeftPose, ControlChannel::RightPose,
				  ControlChannel::LeftControls, ControlChannel::RightControls})
				Require(owners[static_cast<std::size_t>(channel)] &&
							owners[static_cast<std::size_t>(channel)]->kind == CommandSourceKind::Device,
						"Takeover must acquire every headset channel");
		}
		else
			Require(bridge.Status().phase == headset::HeadsetInputPhase::Failed &&
						owners[static_cast<std::size_t>(ControlChannel::Head)] == existing &&
						!owners[static_cast<std::size_t>(ControlChannel::LeftPose)],
					"Normal connection preserves conflicts without partially acquiring other channels");
		bridge.Stop();
		const auto stoppedTiming = bridge.Status();
		Require(stoppedTiming.queueDepth == 0 &&
					stoppedTiming.queuedSamples == stoppedTiming.dequeuedSamples + stoppedTiming.discardedSamples,
				"Stopping input must account for every queued observation without retaining a backlog");
		tick();
	}
	for (bool pause : {false, true})
	{
		auto calibration = IdentityPose();
		calibration.position.x = 2;
		headset::HeadsetInputTarget target{endpoint, 1, session->Snapshot().generation, calibration, 7};
		target.takeOver = true;
		target.displayTiming = true;
		target.trackers = !pause;
		target.alignment.emplace();
		target.alignment->worldHead.position.x = 2;
		target.alignment->localEyeHeight = 1.6f;
		headset::HeadsetInputBridge bridge(target);
		const auto deadline = LiveInputClock::now() + std::chrono::seconds(3);
		while (bridge.Status().phase == headset::HeadsetInputPhase::Connecting && LiveInputClock::now() < deadline)
		{
			tick();
			std::this_thread::sleep_for(std::chrono::milliseconds(2));
		}
		const auto status = bridge.Status();
		Require(status.phase == headset::HeadsetInputPhase::Ready, status.error.c_str());
		Require(!status.worldFromInitial, "Calibration published before any input acknowledgement");
		headset::HeadsetInputSample input;
		input.trackers.emplace();
		for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
		{
			headset::PhysicalTrackerSample physical;
			physical.persistentPath = "/device/" + std::to_string(index);
			physical.location.pose = IdentityPose();
			physical.location.pose.position.x = static_cast<float>(index);
			physical.location.locationFlags = XR_SPACE_LOCATION_POSITION_VALID_BIT;
			input.trackers->push_back(physical);
		}
		input.displayPeriod = 10'000'000;
		input.focused = input.controlsReady = true;
		input.viewsLocated = true;
		for (std::size_t eye = 0; eye < 2; ++eye)
		{
			input.eyes[eye].location.pose = {EulerAngles(eye ? -.1f : .1f, 0, 0), {eye ? .04f : -.03f, 1.6f, 0}};
			input.eyes[eye].location.locationFlags = eye ? 0 : TrackedPoseFlags;
			input.eyes[eye].fov = {-.7f, .8f, .6f, -.9f};
		}
		input.head.pose = IdentityPose();
		input.head.pose.position.y = 1.6f;
		input.head.locationFlags = TrackedPoseFlags;
		input.hands[0].select.isActive = true;
		input.hands[0].select.currentState = true;
		input.hands[0].controls.grip = .75f;
		input.hands[0].controls.gripForce = .6f;
		input.hands[0].controls.trackpad = {-.3f, .8f};
		input.hands[0].controls.trackpadForce = .9f;
		input.hands[0].controls.trackpadTouch = true;
		input.hands[0].controls.stick = {.4f, -.2f};
		input.hands[0].controls.primary = true;
		input.hands[0].controls.triggerTouch = true;
		input.hands[0].controls.triggerClick = false;
		input.hands[0].controls.physicalProfile = "/interaction_profiles/valve/index_controller";

		if (!pause)
		{
			auto unavailable = input;
			unavailable.head.locationFlags = 0;
			const auto first = ReadNativeClockSample();
			Require(bridge.Publish(unavailable, first.domain, first.nanoseconds, first.nanoseconds + 10000000),
					"Wait for initial tracking without applying an unavailable head pose");
		}
		if (pause)
			session->Scheduler().Pause();
		const auto now = ReadNativeClockSample();
		const bool queued = bridge.Publish(input, now.domain, now.nanoseconds, now.nanoseconds + 10000000);
		Require(queued || (pause && bridge.Status().phase == headset::HeadsetInputPhase::Suspended),
				"Queue physical observation or observe clock suspension");
		while (!bridge.Status().sent && bridge.Status().phase == headset::HeadsetInputPhase::Ready &&
			   LiveInputClock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		if (pause)
		{
			Require(bridge.Status().phase == headset::HeadsetInputPhase::Suspended,
					"Pause suspends input without a fatal host failure");
			for (int index = 0; index < 100; ++index)
				Require(bridge.Publish(input, now.domain, now.nanoseconds, now.nanoseconds + 10000000),
						"Suspended observations coalesce instead of overflowing an edge queue");
			Require(session->Scheduler().Resume(LiveInputClock::now(), session->Scheduler().Snapshot().nextAdmission) ==
						XR_SUCCESS,
					"Resume simulator clock");
			for (int index = 0; index < 20; ++index)
			{
				const auto stamp = ReadNativeClockSample();
				bridge.Publish(input, stamp.domain, stamp.nanoseconds, stamp.nanoseconds + 10000000);
				tick();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Require(bridge.Status().phase == headset::HeadsetInputPhase::Suspended && bridge.Status().sent == 0,
					"Held controls cannot resume forwarding after a new clock mapping");
			auto neutral = input;
			for (auto& hand : neutral.hands)
			{
				hand.controls = {};
				hand.select.currentState = false;
				hand.trigger.currentState = 0;
			}
			while (!session->Commands().Snapshot().applied->owners[static_cast<std::size_t>(ControlChannel::Head)] &&
				   LiveInputClock::now() < deadline)
			{
				const auto stamp = ReadNativeClockSample();
				bridge.Publish(neutral, stamp.domain, stamp.nanoseconds, stamp.nanoseconds + 10000000);
				tick();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			for (int index = 0; index < 20; ++index)
			{
				const auto stamp = ReadNativeClockSample();
				bridge.Publish(input, stamp.domain, stamp.nanoseconds, stamp.nanoseconds + 10000000);
				tick();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Require(bridge.Status().phase == headset::HeadsetInputPhase::Suspended && bridge.Status().sent == 0,
					"A press during ownership recovery still requires a fresh neutral boundary");
			while (!bridge.Status().sent && LiveInputClock::now() < deadline)
			{
				const auto stamp = ReadNativeClockSample();
				bridge.Publish(neutral, stamp.domain, stamp.nanoseconds, stamp.nanoseconds + 10000000);
				tick();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Require(bridge.Status().phase == headset::HeadsetInputPhase::Ready && bridge.Status().sent > 0,
					bridge.Status().error.c_str());
			tick();
			Require(session->Input().Snapshot().controllers[0].trigger == 0,
					"Resume publishes neutral input, not the controls held through pause");
			const auto sent = bridge.Status().sent;
			while (bridge.Status().sent == sent && LiveInputClock::now() < deadline)
			{
				const auto stamp = ReadNativeClockSample();
				bridge.Publish(input, stamp.domain, stamp.nanoseconds, stamp.nanoseconds + 10000000);
				tick();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			tick();
			Require(session->Input().Snapshot().controllers[0].trigger == 1,
					"New control presses reach the game after neutral recovery");
		}
		else
		{
			Require(bridge.Status().sent == 1, "Forward sample over production local transport");
			const auto timing = bridge.Status();
			Require(timing.queuedSamples >= timing.dequeuedSamples && timing.dequeuedSamples >= timing.sent &&
						timing.queueDepth <= timing.queueHighWater && timing.queueHighWater > 0 &&
						timing.queueHighWater <= 64 && timing.maximumQueueDelay.count() >= 0 &&
						timing.maximumForwardAge.count() > 0,
					"Input transport did not retain bounded queue and forwarding timing evidence");
			Require(bridge.Status().predictions == 1,
					"Host prediction reaches coordinated pacing through production transport");
			const auto calibration = bridge.Status().worldFromInitial;
			Require(calibration && calibration->position.x == 2,
					"Output did not receive acknowledged input calibration");
			tick();
			const auto applied = session->Commands().Snapshot().applied;
			const auto& trackers = applied->configuration.tracking.trackers;
			Require(trackers.size() == MaxTrackerDevices && trackers.back().sourcePath.View() == "htcx:/device/63" &&
						trackers.back().poseOverride->position.x == 65 &&
						trackers.back().poseFlags == XR_SPACE_LOCATION_POSITION_VALID_BIT &&
						applied->owners[static_cast<std::size_t>(ControlChannel::Trackers)].has_value(),
					"Calibrated tracker collection did not reach owned runtime state");
			const auto& eyes = applied->configuration.tracking.eyeViews;
			Require(eyes && eyes->spaceRevision == 7 && eyes->poses[0].pose.position.x == 1.97f &&
						eyes->poses[0].pose.orientation.y > 0 && eyes->poses[1].flags == 0 &&
						eyes->fovs[1].angleDown == -.9f,
					"Calibrated physical eye geometry and validity reach the runtime over local transport");
			const auto controls = session->Input().Snapshot().controllers[0];
			Require(controls.physicalProfile == "/interaction_profiles/valve/index_controller" &&
						controls.trigger == 1 && controls.triggerClick == false && controls.grip == .75f &&
						controls.stick.x == .4f && controls.stick.y == -.2f && controls.primary &&
						controls.triggerTouch && controls.gripForce == .6f && controls.trackpad.x == -.3f &&
						controls.trackpad.y == .8f && controls.trackpadForce == .9f && controls.trackpadTouch,
					"Physical analog, button and touch observations reach runtime controls");
			const auto firstTracker = trackers.front().identity;
			const auto publishTrackers = [&](const headset::HeadsetInputSample& changed) {
				const auto sent = bridge.Status().sent;
				const auto stamp = ReadNativeClockSample();
				Require(bridge.Publish(changed, stamp.domain, stamp.nanoseconds, stamp.nanoseconds + 10000000),
						"Queue changed physical tracker inventory");
				while (bridge.Status().sent == sent && LiveInputClock::now() < deadline)
					std::this_thread::sleep_for(std::chrono::milliseconds(1));
				Require(bridge.Status().sent > sent, bridge.Status().error.c_str());
				session->Commands().AcknowledgeLiveInput();
				tick();
				return session->Commands().Snapshot().applied;
			};
			auto disconnected = input;
			disconnected.trackers->clear();
			Require(publishTrackers(disconnected)->configuration.tracking.trackers.empty(),
					"Physical disconnect left phantom tracker devices");
			input.trackers->front().role = TrackerRole::Waist;
			input.trackers->at(1).role = TrackerRole::Waist;
			const auto reconnected = publishTrackers(input);
			Require(reconnected->configuration.tracking.trackers.front().identity == firstTracker &&
						reconnected->configuration.tracking.trackers.front().role == TrackerRole::Waist &&
						reconnected->configuration.tracking.trackers.at(1).role == TrackerRole::Waist &&
						reconnected->configuration.tracking.trackers.at(1).identity != firstTracker,
					"Physical reconnect or reassignment changed persistent handle");
			const std::array devices{HapticDevice::LeftController};
			XrHapticVibration vibration{XR_TYPE_HAPTIC_VIBRATION};
			vibration.amplitude = .6f;
			vibration.frequency = 80;
			vibration.duration = 200'000'000;
			const auto hapticTime = session->ObserveHaptics(session->Snapshot().generation)->time;
			Require(session->Haptics().Apply(devices, reinterpret_cast<XrAction>(1), XR_NULL_PATH, vibration,
											 hapticTime, true) == XR_SUCCESS,
					"Emit application haptic event");
			std::optional<headset::HeadsetVibration> output;
			while (!output && LiveInputClock::now() < deadline)
			{
				const auto stamp = ReadNativeClockSample();
				bridge.Publish(input, stamp.domain, stamp.nanoseconds, stamp.nanoseconds + 10000000);
				output = bridge.ConsumeHaptics(true)[0];
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Require(output && output->amplitude == .6f && output->frequency == 80 && output->remaining > 0 &&
						output->remaining <= vibration.duration,
					"Production control polling delivers bounded application feedback");
			Require(!bridge.ConsumeHaptics(true)[0], "Polling does not replay already consumed output");
			session->Scheduler().Pause();
			while (bridge.Status().phase == headset::HeadsetInputPhase::Ready && LiveInputClock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			Require(bridge.Status().phase == headset::HeadsetInputPhase::Suspended,
					"Clock polling detects pause even without another physical observation");
			Require(session->Scheduler().Resume(LiveInputClock::now(), session->Scheduler().Snapshot().nextAdmission) ==
						XR_SUCCESS,
					"Resume simulator clock");
			claim.id = 2;
			claim.target.frame = FrameId{frame + 1};
			claim.commands = {TakeControl{ControlChannel::Head}};
			Require(session->Commands().Submit(claim).result == CommandResult::Queued, "Claim head during suspension");
			tick();
			auto neutral = input;
			for (auto& hand : neutral.hands)
			{
				hand.controls = {};
				hand.select.currentState = false;
				hand.trigger.currentState = 0;
			}
			while (bridge.Status().error.find("ownership unavailable") == std::string::npos &&
				   bridge.Status().phase == headset::HeadsetInputPhase::Suspended && LiveInputClock::now() < deadline)
			{
				const auto stamp = ReadNativeClockSample();
				bridge.Publish(neutral, stamp.domain, stamp.nanoseconds, stamp.nanoseconds + 10000000);
				tick();
				std::this_thread::sleep_for(std::chrono::milliseconds(1));
			}
			Require(bridge.Status().phase == headset::HeadsetInputPhase::Suspended &&
						bridge.Status().error.find("ownership unavailable") != std::string::npos,
					"Recovery conflict keeps inspection alive and requires explicit retry");
			const auto owners = session->Commands().Snapshot().applied->owners;
			Require(owners[static_cast<std::size_t>(ControlChannel::Head)] == existing &&
						!owners[static_cast<std::size_t>(ControlChannel::LeftControls)],
					"Resume never reuses initial takeover authorization or partially claims channels");
		}
		bridge.Stop();
		const auto stoppedTiming = bridge.Status();
		Require(stoppedTiming.queueDepth == 0 &&
					stoppedTiming.queuedSamples == stoppedTiming.dequeuedSamples + stoppedTiming.discardedSamples,
				"Stopping input must account for every queued observation without retaining a backlog");
		if (!pause)
		{
			const auto stopped = bridge.ConsumeHaptics(true);
			Require(stopped[0] && stopped[0]->amplitude == 0, "Bridge stop emits physical output cancellation");
		}
		while (session->Input().Snapshot().controllers[0].trigger != 0 && LiveInputClock::now() < deadline)
			std::this_thread::sleep_for(std::chrono::milliseconds(1));
		Require(session->Input().Snapshot().controllers[0].trigger == 0, "Worker stop releases held control");
		tick();
		Require(!session->Commands().Snapshot().applied->configuration.tracking.eyeViews,
				"Worker stop releases physical view ownership");
	}
	server.Close();
	session->Close();
}
} // namespace simxr::tests

int main()
{
	try
	{
		simxr::tests::NeutralRestart();
		for (const auto set : {simxr::BodyJointSet::UpperBodyFB, simxr::BodyJointSet::FullBodyMETA,
							   simxr::BodyJointSet::WithoutArmsBD, simxr::BodyJointSet::FullBodyBD})
			simxr::tests::NeutralRestart(set);
		simxr::tests::NeutralRestart(simxr::BodyJointSet::UpperBodyFB, false);
		simxr::tests::NeutralRestart(simxr::BodyJointSet::UpperBodyFB, true, true);
		simxr::tests::Forwarding();
		std::cout << "Native input worker checks passed\n";
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
