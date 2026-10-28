#include "../TestSupport.h"
#include "app/services/HeadsetHost.h"
#include "app/services/SimulationEdits.h"
#include "math/Math.h"
#include "simulation/CommandScheduler.h"

#include <charconv>
#include <chrono>
#include <cmath>
#include <iostream>
#include <limits>
#include <numbers>
#include <string>
#include <thread>

#ifdef _WIN32
#include <cstdio>
#include <fcntl.h>
#include <io.h>
#endif

using namespace simxr::app;

static HeadsetHostSnapshot Wait(HeadsetHost& host, bool terminal)
{
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(10);
	while (std::chrono::steady_clock::now() < deadline)
	{
		auto state = host.Snapshot();
		if (terminal ? state.phase == HeadsetHostPhase::Exited || state.phase == HeadsetHostPhase::Failed
					 : state.diagnostics.find("ready") != std::string::npos)
			return state;
		std::this_thread::sleep_for(std::chrono::milliseconds(5));
	}
	throw std::runtime_error("Host service did not reach expected state");
}
int main(int argc, char** argv)
{
#ifdef _WIN32
	if (_setmode(_fileno(stdout), _O_BINARY) == -1)
		return 1;
#endif
	if (argc > 1 && std::string(argv[1]) == "--discover")
	{
		if (argc != 4 || std::string(argv[2]) != "--parent-control" || std::string(argv[3]) != "--system")
			return 14;
		std::cout << "simxr_host_eye_sizes=2000,2100,2200,2300\nready\n" << std::flush;
		char value;
		std::cin.get(value);
		return 0;
	}
	if (argc > 1 && std::string(argv[1]) == "--run")
	{
		if (argc < 9 || std::string(argv[2]) != "--parent-control" || std::string(argv[3]) != "--forward-input" ||
			std::string(argv[7]) != "--game-output")
			return 10;
		const std::string mode = argv[4];
		if (mode != "realign" && (std::string(argv[argc - 1]) == "--take-over") != (mode == "takeover"))
			return 13;
		if (mode == "realign")
		{
			bool takeover = false;
			for (int index = 7; index < argc; ++index)
			{
				if (std::string(argv[index]) == "--take-over")
					takeover = true;
				if (std::string(argv[index]) == "--align-player" && index + 4 < argc)
					std::cout << "alignment_x=" << argv[index + 1] << '\n';
			}
			std::cout << "takeover=" << takeover << '\n';
		}
		if (mode == "alignment")
		{
			if (argc != 17 || std::string(argv[8]) != "--runtime" ||
				std::string(argv[9]) != "runtime with spaces.json" || std::string(argv[10]) != "--align-player" ||
				std::string(argv[15]) != "--local-eye-height")
				return 11;
			const float expected[] = {std::nextafter(2.f, 3.f), -3.5f, .785398185f, .25f, 1.654321f};
			for (unsigned index = 0; index < 5; ++index)
			{
				const std::string text = argv[index < 4 ? 11 + index : 16];
				float value = 0;
				const auto parsed = std::from_chars(text.data(), text.data() + text.size(), value);
				if (parsed.ec != std::errc{} || parsed.ptr != text.data() + text.size() || value != expected[index])
					return 12;
			}
		}
		if (mode == "eye-sizes")
		{
			std::cout << "simxr_host_eye_sizes=2048,2160," << std::flush;
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			std::cout << "2200,2300\r\n";
			std::cout << "simxr_host_eye_sizes=1,2,3,4junk\n"
					  << "simxr_host_eye_sizes=1,2,3\n"
					  << "simxr_host_eye_sizes=1,2,3,4,5\n"
					  << "simxr_host_eye_sizes=0,2,3,4\n"
					  << "simxr_host_eye_sizes=-1,2,3,4\n"
					  << "simxr_host_eye_sizes=4294967296,2,3,4\n"
					  << "simxr_host_eye_sizes=1, 2,3,4\n"
					  << std::string(129, 'x') << "simxr_host_eye_sizes=1,2,3,4\n";
		}
		if (mode.starts_with("body-"))
		{
			const auto expected = mode.substr(5);
			bool selected = false;
			for (int index = 8; index < argc; ++index)
				if (std::string(argv[index]) == "--forward-body")
				{
					if (++index >= argc || argv[index] != expected || selected)
						return 15;
					selected = true;
				}
			if (selected != (expected != "off"))
				return 16;
			std::cout << "body_support_fb=" << std::flush;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			std::cout << "1\r\nbody_support_meta_full_body=0\nbody_support_bd=1\n"
					  << "body_support_fb=0junk\nbody_support_bd=unknown\n"
					  << std::string(129, 'x') << "body_support_fb=0\n";
		}
		if (mode == "no-trackers")
			std::cout << "simxr_host_tracker_support=0\nsimxr_host_tracker_count=0\n" << std::flush;
		if (mode == "trackers")
		{
			std::cout << "simxr_host_tracker_support=" << std::flush;
			std::this_thread::sleep_for(std::chrono::milliseconds(10));
			std::cout << "1\r\nsimxr_host_tracker_count=0\n"
					  << "simxr_host_tracker_count=65\n"
					  << "simxr_host_tracker_count=-1\n"
					  << "simxr_host_tracker_count=1junk\n"
					  << "simxr_host_tracker_support=unknown\n"
					  << std::string(129, 'x') << "simxr_host_tracker_count=4\n"
					  << std::flush;
		}
		if (mode == "status")
		{
			std::cout << std::string(256, 'x') << "simxr_host_input=ready\n" << "simxr_host_input=" << std::flush;
			std::this_thread::sleep_for(std::chrono::milliseconds(150));
			std::cout << "ready\r\nsimxr_host_output=live\n" << std::flush;
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			std::cout << "simxr_host_input=suspended\nsimxr_host_output=inspection\n" << std::flush;
			std::this_thread::sleep_for(std::chrono::milliseconds(200));
			std::cout << "simxr_host_input=ready\nsimxr_host_output=hidden\n" << std::flush;
			for (int index = 0; index < 4; ++index)
			{
				std::this_thread::sleep_for(std::chrono::milliseconds(150));
				std::cout << "simxr_host_input=ready\n" << std::flush;
			}
		}
		if (mode == "flood")
		{
			std::cout << std::string(200000, 'x') << "tail-marker\n" << std::flush;
			return 7;
		}
		std::cout << "ready\n" << std::flush;
		if (mode == "stalled")
			std::this_thread::sleep_for(std::chrono::seconds(60));
		else
		{
			char value;
			std::cin.get(value);
		}
		std::cerr << "closed\n" << std::flush;
		return 0;
	}
	try
	{
		const auto executable = std::filesystem::absolute(argv[0]);
		ObservedSetup ownership;
		const simxr::CommandSource manual{simxr::CommandSourceKind::Manual, 77};
		ownership.owners.fill(manual);
		const auto release = PrepareHeadsetRelease(ownership, manual);
		Require(release.commands.size() == 6, "Headset handoff releases exactly its six input channels");
		for (const auto& command : release.commands)
		{
			const auto* value = std::get_if<simxr::ReleaseControl>(&command);
			Require(value && value->channel != simxr::ControlChannel::Body &&
						value->channel != simxr::ControlChannel::Gaze &&
						value->channel != simxr::ControlChannel::Trackers,
					"Handoff must preserve unrelated simulated channels");
			const auto slot = static_cast<std::size_t>(value->channel);
			ownership.owners[slot] = simxr::CommandSource{simxr::CommandSourceKind::Rig, 81};
			bool rejected = false;
			try
			{
				PrepareHeadsetRelease(ownership, manual);
			}
			catch (const std::runtime_error& error)
			{
				rejected = std::string(error.what()).find("81") != std::string::npos;
			}
			Require(rejected, "Each foreign-owned headset channel must report its source instead of replacing it");
			ownership.owners[slot] = manual;
		}
		Require(PrepareHeadsetRelease(ownership, manual, false, true).commands.size() == 7,
				"Selected trackers were omitted from manual handoff");
		const auto trackerSlot = static_cast<std::size_t>(simxr::ControlChannel::Trackers);
		ownership.owners[trackerSlot] = simxr::CommandSource{simxr::CommandSourceKind::Rig, 82};
		Require(PrepareHeadsetRelease(ownership, manual).commands.size() == 6,
				"Unselected foreign tracker ownership blocked head-only connection");
		bool trackerConflict = false;
		try
		{
			PrepareHeadsetRelease(ownership, manual, false, true);
		}
		catch (const std::runtime_error&)
		{
			trackerConflict = true;
		}
		Require(trackerConflict && PrepareHeadsetRelease(ownership, manual, true, true).commands.size() == 6,
				"Selected tracker conflict did not require explicit takeover");
		ownership.owners[trackerSlot] = manual;
		Require(PrepareHeadsetRelease(ownership, manual, false, true, true).commands.size() == 8,
				"Selected body and trackers must join manual handoff");
		const auto bodySlot = static_cast<std::size_t>(simxr::ControlChannel::Body);
		ownership.owners[bodySlot] = simxr::CommandSource{simxr::CommandSourceKind::Rig, 83};
		Require(PrepareHeadsetRelease(ownership, manual).commands.size() == 6,
				"Unselected body owner must not block connection");
		bool bodyConflict = false;
		try
		{
			PrepareHeadsetRelease(ownership, manual, false, false, true);
		}
		catch (const std::runtime_error& error)
		{
			bodyConflict = std::string(error.what()).find("83") != std::string::npos;
		}
		Require(bodyConflict && PrepareHeadsetRelease(ownership, manual, true, false, true).commands.size() == 6,
				"Body takeover must be explicit and identify the conflicting owner");
		ownership.owners[bodySlot] = manual;
		for (const bool raced : {false, true})
		{
			simxr::CommandScheduler scheduler;
			simxr::CommandConfiguration baseline;
			scheduler.Start(0, baseline);
			simxr::CommandTransaction claim;
			claim.id = 1;
			claim.source = manual;
			claim.target.frame = simxr::FrameId{1};
			for (std::size_t channel = 0; channel < simxr::ControlChannelCount; ++channel)
				claim.commands.push_back(simxr::TakeControl{static_cast<simxr::ControlChannel>(channel)});
			Require(scheduler.Submit(claim).result == simxr::CommandResult::Queued, "Queue manual ownership fixture");
			scheduler.Apply({1}, {simxr::Duration{1}}, {simxr::Duration{1}}, baseline);
			const simxr::CommandSource other{simxr::CommandSourceKind::Rig, 81};
			if (raced)
			{
				simxr::CommandTransaction change;
				change.id = 1;
				change.source = other;
				change.target.frame = simxr::FrameId{2};
				change.commands = {simxr::TakeControl{simxr::ControlChannel::LeftControls, true}};
				Require(scheduler.Submit(change).result == simxr::CommandResult::Queued, "Queue ownership race");
			}
			claim.id = 2;
			claim.target.frame = simxr::FrameId{2};
			claim.commands = release.commands;
			Require(scheduler.Submit(claim).result == simxr::CommandResult::Queued, "Queue selective release");
			const auto applied = scheduler.Apply({2}, {simxr::Duration{2}}, {simxr::Duration{2}}, baseline);
			for (const auto& command : release.commands)
			{
				const auto channel = std::get<simxr::ReleaseControl>(command).channel;
				const auto owner = applied->owners[static_cast<std::size_t>(channel)];
				Require(raced ? owner == (channel == simxr::ControlChannel::LeftControls ? other : manual) : !owner,
						"Selective release must apply atomically or preserve every owner on conflict");
			}
			Require(applied->owners[static_cast<std::size_t>(simxr::ControlChannel::Body)] == manual &&
						applied->owners[static_cast<std::size_t>(simxr::ControlChannel::Gaze)] == manual,
					"Handoff must retain body and gaze ownership in the real scheduler");
		}
		ownership.owners.fill(std::nullopt);
		ownership.owners[static_cast<std::size_t>(simxr::ControlChannel::Body)] =
			simxr::CommandSource{simxr::CommandSourceKind::Rig, 81};
		Require(PrepareHeadsetRelease(ownership, std::nullopt).commands.empty(),
				"An unrelated body owner does not block the headset");
		ObservedSetup observed;
		observed.values.tracking.head = {{simxr::EulerAngles(.7f, .2f, .1f), {12, 3, -5}}, simxr::TrackedPoseFlags};
		observed.values.tracking.body.rootPose = XrPosef{{0, 0, 0, 1}, {10, 1.25f, -4}};
		const auto aligned = HeadsetAlignmentFor(observed);
		Require(aligned.x == 12 && aligned.z == -5 && std::abs(aligned.yaw - .7f) < .0001f && aligned.floor == 1.25f,
				"Alignment uses applied head position/facing and player floor, not body facing or eye height");
		observed.values.tracking.body.rootPose.reset();
		Require(HeadsetAlignmentFor(observed).floor == 0, "An absent body root retains the simulator world floor");
		for (unsigned invalid = 0; invalid < 3; ++invalid)
		{
			auto bad = observed;
			if (invalid == 0)
				bad.values.tracking.head.flags = 0;
			else if (invalid == 1)
				bad.values.tracking.head.pose.orientation = simxr::EulerAngles(0, std::numbers::pi_v<float> / 2, 0);
			else
				bad.values.tracking.head.pose.position.x = std::numeric_limits<float>::quiet_NaN();
			bool rejected = false;
			try
			{
				HeadsetAlignmentFor(bad);
			}
			catch (const std::invalid_argument&)
			{
				rejected = true;
			}
			Require(rejected, "Unavailable, vertical or nonfinite head cannot establish alignment");
		}
		HeadsetHost host;
		HeadsetHostOptions takeover{executable, {}, "takeover", 1, 1};
		takeover.takeOver = true;
		host.Start(takeover);
		Wait(host, false);
		host.Stop();
		Require(Wait(host, true).phase == HeadsetHostPhase::Exited,
				"Explicit takeover flag must reach the owned child");
		for (const bool selected : {false, true})
		{
			HeadsetHostOptions trackerOptions{executable, {}, "trackers", 1, 1};
			trackerOptions.trackers = selected;
			host.Start(trackerOptions);
			const auto trackerStatus = Wait(host, false);
			Require(trackerStatus.trackerSupport == true && trackerStatus.connectedTrackers.has_value() == selected &&
						(!selected || *trackerStatus.connectedTrackers == 0),
					"Tracker support, zero inventory or malformed child status was misread");
			if (selected)
			{
				const auto expiry = std::chrono::steady_clock::now() + std::chrono::seconds(2);
				while (host.Snapshot().connectedTrackers && std::chrono::steady_clock::now() < expiry)
					std::this_thread::sleep_for(std::chrono::milliseconds(5));
				Require(!host.Snapshot().connectedTrackers && host.Snapshot().trackerSupport == true,
						"Stale inventory remained live or erased stable runtime capability");
			}
			host.Stop();
			const auto stopped = Wait(host, true);
			Require(!stopped.connectedTrackers && !stopped.trackerSupport,
					"Disconnected host retained physical tracker status");
		}
		for (int selection = -1; selection < 4; ++selection)
		{
			const auto name = selection < 0 ? std::string_view("off") : simxr::BodyJointSetNames[selection];
			HeadsetHostOptions options{executable, {}, "body-" + std::string(name), 1, 1};
			if (selection >= 0)
				options.body = static_cast<simxr::BodyJointSet>(selection);
			host.Start(options);
			const auto status = Wait(host, false);
			Require(status.bodySupport[0] == true && status.bodySupport[1] == false && status.bodySupport[2] == true,
					"Body provider capabilities must survive split and malformed status lines");
			host.Stop();
			const auto stopped = Wait(host, true);
			Require(stopped.phase == HeadsetHostPhase::Exited &&
						std::all_of(stopped.bodySupport.begin(), stopped.bodySupport.end(),
									[](auto value)
									{
										return !value;
									}),
					"Body selection must reach the child and capability status must clear on disconnect");
		}
		HeadsetHostOptions unsupportedTrackers{executable, {}, "no-trackers", 1, 1};
		unsupportedTrackers.trackers = true;
		host.Start(unsupportedTrackers);
		const auto unsupported = Wait(host, false);
		Require(unsupported.trackerSupport == false && !unsupported.connectedTrackers,
				"Unsupported tracker provider was presented as a connected empty inventory");
		host.Stop();
		Wait(host, true);
		HeadsetHostOptions realignOptions{executable, {}, "realign", 1, 1};
		realignOptions.takeOver = true;
		host.Start(realignOptions);
		Wait(host, false);
		Require(host.Snapshot().diagnostics.find("takeover=1") != std::string::npos,
				"Initial connection retains explicit takeover");
		host.Realign({9, 2, .5f, 0});
		const auto realignDeadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
		while (host.Restarting() && std::chrono::steady_clock::now() < realignDeadline)
		{
			host.ObserveTarget("realign", 1, 1, 0);
			std::this_thread::sleep_for(std::chrono::milliseconds(5));
		}
		Require(!host.Restarting(), "Clean exit must finish the queued reconnect");
		Wait(host, false);
		Require(host.Snapshot().phase == HeadsetHostPhase::Running &&
					host.Snapshot().diagnostics.find("alignment_x=9") != std::string::npos &&
					host.Snapshot().diagnostics.find("takeover=0") != std::string::npos,
				"Re-alignment starts a fresh child with the captured pose and without reusing takeover");
		host.Realign({10, 0, 0, 0});
		host.ObserveTarget("different-target", 1, 1, 0);
		Require(!host.Restarting(), "Target change must cancel queued realignment");
		Wait(host, true);
		HeadsetHostOptions alignedOptions{executable, "runtime with spaces.json", "alignment", 1, 1};
		alignedOptions.alignment = HeadsetHostAlignment{std::nextafter(2.f, 3.f), -3.5f, .785398185f, .25f};
		alignedOptions.localEyeHeight = 1.654321f;
		host.Start(alignedOptions);
		Wait(host, false);
		host.Stop();
		Require(Wait(host, true).phase == HeadsetHostPhase::Exited,
				"Alignment values round-trip exactly through real child arguments, retaining runtime path spaces");
		for (const float height : {0.f, -1.f, std::numeric_limits<float>::infinity()})
		{
			alignedOptions.localEyeHeight = height;
			bool rejected = false;
			try
			{
				host.Start(alignedOptions);
			}
			catch (const std::invalid_argument&)
			{
				rejected = true;
			}
			Require(rejected, "Invalid eye-height calibration is rejected before process launch");
		}
		host.Start({executable, {}, "cooperative", 1, 1});
		Wait(host, false);
		bool rejected = false;
		try
		{
			host.Start({executable, {}, "cooperative", 1, 1});
		}
		catch (const std::logic_error&)
		{
			rejected = true;
		}
		Require(rejected, "Reject second live host");
		host.Stop();
		auto state = Wait(host, true);
		Require(state.phase == HeadsetHostPhase::Exited && !state.forcedStop &&
					state.diagnostics.find("closed") != std::string::npos,
				"Pipe closure requests graceful shutdown and captures stderr");
		for (unsigned change = 0; change < 5; ++change)
		{
			host.Start({executable, {}, "cooperative", 1, 2, 3});
			Wait(host, false);
			host.ObserveTarget("cooperative", 1, 2, 3);
			std::this_thread::sleep_for(std::chrono::milliseconds(20));
			Require(host.Snapshot().phase == HeadsetHostPhase::Running,
					"An unchanged target keeps its owned host running");
			host.ObserveTarget(change == 0	 ? "other-endpoint"
							   : change == 4 ? ""
											 : "cooperative",
							   change == 1 ? 10 : 1, change == 2 ? 20 : 2, change == 3 ? 30 : 3);
			state = Wait(host, true);
			Require(state.phase == HeadsetHostPhase::Exited && !state.forcedStop,
					"Endpoint, session, run, reconnect epoch and disconnection each stop the old host");
			host.ObserveTarget("cooperative", 1, 2, 3);
			Require(host.Snapshot().phase == HeadsetHostPhase::Exited,
					"Returning to an old target does not automatically restart the host");
		}
		HeadsetHostOptions discovery;
		discovery.executable = executable;
		discovery.discover = true;
		host.Start(discovery);
		const auto detected = Wait(host, false);
		Require(detected.discovering && detected.recommendedEyes && detected.input == HeadsetInputState::Waiting &&
					detected.output == HeadsetOutputState::Waiting,
				"Discovery fabricated an input/output attachment");
		host.ObserveTarget("new-game", 2, 3, 4);
		Require(host.Snapshot().phase == HeadsetHostPhase::Running,
				"Game discovery stopped independent headset discovery");
		bool rejectedRealign = false;
		try
		{
			host.Realign({});
		}
		catch (const std::logic_error&)
		{
			rejectedRealign = true;
		}
		Require(rejectedRealign, "Discovery queued a targetless input realignment");
		host.Stop();
		Require(!Wait(host, true).recommendedEyes, "Discovery retained recommendations after disconnect");
		for (const bool changeTarget : {false, true})
		{
			host.Start(discovery);
			Wait(host, false);
			host.Start({executable, {}, "connected-from-discovery", 8, 9});
			Require(host.Restarting(), "Discovery connection did not wait for its old process");
			if (changeTarget)
			{
				host.ObserveTarget("different", 8, 9, 0);
				Require(!host.Restarting(), "Target change retained a queued discovery connection");
				Wait(host, true);
			}
			else
			{
				Eventually(
					[&]
					{
						host.ObserveTarget("connected-from-discovery", 8, 9, 0);
						const auto state = host.Snapshot();
						return !host.Restarting() && !state.discovering && state.phase == HeadsetHostPhase::Running &&
							   state.diagnostics.find("ready") != std::string::npos;
					},
					"Discovery did not transition to the explicit simulator target");
				host.Stop();
				Wait(host, true);
			}
		}
		host.Start({executable, {}, "eye-sizes", 1, 1});
		const auto recommendations = Wait(host, false);
		Require(recommendations.recommendedEyes == std::array<HeadsetEyeSize, 2>{{{2048, 2160}, {2200, 2300}}},
				"Host lost asymmetric eye recommendations or accepted malformed updates");
		host.Stop();
		Require(!Wait(host, true).recommendedEyes, "Stopped host retained stale eye recommendations");
		host.Start({executable, {}, "status", 1, 1});
		Wait(host, false);
		Require(host.Snapshot().input == HeadsetInputState::Waiting,
				"An overlong line or incomplete marker cannot claim readiness");
		const auto waitInput = [&](HeadsetInputState input)
		{
			const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds(3);
			while (host.Snapshot().input != input && std::chrono::steady_clock::now() < deadline)
				std::this_thread::sleep_for(std::chrono::milliseconds(5));
			Require(host.Snapshot().input == input, "Host input status did not reach the expected state");
		};
		waitInput(HeadsetInputState::Ready);
		Require(host.Snapshot().output == HeadsetOutputState::Live,
				"Live output status is independent of process startup");
		waitInput(HeadsetInputState::Suspended);
		Require(host.Snapshot().output == HeadsetOutputState::Inspection, "Suspended game reports inspection screen");
		waitInput(HeadsetInputState::Ready);
		Require(host.Snapshot().output == HeadsetOutputState::Hidden,
				"Physical render suppression is distinct from input readiness");
		Eventually(
			[&]
			{
				return host.Snapshot().output == HeadsetOutputState::Waiting;
			},
			"Missing output heartbeat expires independently");
		Require(host.Snapshot().input == HeadsetInputState::Ready,
				"Fresh input cannot refresh or conceal an expired output heartbeat");
		waitInput(HeadsetInputState::Waiting);
		Require(host.Snapshot().output == HeadsetOutputState::Waiting,
				"Stale output heartbeat cannot retain live readiness");
		Require(host.Snapshot().phase == HeadsetHostPhase::Running, "Expired status must not fabricate process exit");
		host.Stop();
		Require(Wait(host, true).input == HeadsetInputState::Waiting, "Stopped host retained input readiness");
		host.Start({executable, {}, "flood", 1, 1});
		state = Wait(host, true);
		Require(state.phase == HeadsetHostPhase::Failed && state.exitCode == 7 && state.diagnostics.size() <= 65536 &&
					state.diagnostics.find("tail-marker") != std::string::npos,
				"Bound diagnostics without deadlocking child or losing final output");
		host.Start({executable, {}, "stalled", 1, 1});
		Wait(host, false);
		host.Realign({});
		state = Wait(host, true);
		host.ObserveTarget("stalled", 1, 1, 0);
		Require(!host.Restarting(), "Forced shutdown must cancel queued realignment");
		Require(state.forcedStop && state.phase == HeadsetHostPhase::Failed && !state.error.empty(),
				"Force stop unresponsive vendor process");
		host.Start({executable.parent_path() / "missing-headset-host", {}, "cooperative", 1, 1});
		Require(Wait(host, true).phase == HeadsetHostPhase::Failed, "Publish launch failure");
		{
			HeadsetHost owned;
			owned.Start({executable, {}, "cooperative", 1, 1});
			Wait(owned, false);
		}
		std::cout << "Headset process ownership checks passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
