#include "../TestSupport.h"
#include "app/services/LibraryCatalog.h"
#include "app/services/RecordingCapture.h"
#include "app/services/RecordingController.h"
#include "app/services/Utf8Path.h"
#include "app/services/WorkerConnection.h"
#include "automation/RecordingArchive.h"
#include "automation/RecordingRange.h"
#include "automation/RigTypes.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "math/Math.h"
#include "runtime/Runtime.h"
#include "simulation/BodyModel.h"
#ifdef SIMXR_TEST_AUTOMATION
#include "automation/RigRunner.h"
#endif

#include <algorithm>
#include <atomic>
#include <csignal>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>
#ifndef _WIN32
#include <sys/resource.h>
#include <sys/wait.h>
#endif

namespace simxr::recording_tests
{
using namespace simxr;
namespace wire = control;
namespace app = simxr::app;

class RecordingHost
{
public:
	explicit RecordingHost(ExecutionMode mode = ExecutionMode::Deterministic)
	{
		static std::atomic<std::uint64_t> s_serial{0};
		const auto name = "simxr-recording-" +
						  std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()) + "-" +
						  std::to_string(++s_serial);
		path = std::filesystem::temp_directory_path() / name;
		Require(std::filesystem::create_directory(path), "Create recording test directory");
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		std::strcpy(create.applicationInfo.applicationName, "CPU recording workflow");
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
		Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
					runtime.CreateSession(instance, session) == XR_SUCCESS &&
					session->ConfigureCapturePool(FramePool{8}, true) == XR_SUCCESS,
				"Create recording session");
		FrameSchedulerConfig config;
		config.mode = mode;
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(config.epoch) == XR_SUCCESS && session->Begin(begin, config) == XR_SUCCESS,
				"Begin recording session");
#ifdef _WIN32
		const auto endpoint = std::string(R"(\\.\pipe\SimXR.Control.)") + name;
#else
		const auto endpoint = (path / "control.sock").string();
#endif
		server = std::make_unique<wire::LocalControlServer>(service, endpoint);
	}

	~RecordingHost()
	{
		server.reset();
		service.Close();
		runtime.Close();
		std::error_code error;
		std::filesystem::remove_all(path, error);
	}

	app::RecordingTarget Target() const
	{
		return {server->Path(), 1, session->Snapshot().generation.value};
	}

	void Frame()
	{
		FramePrediction prediction;
		Require(session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS &&
					session->EndFrame(prediction) == XR_SUCCESS,
				"Publish recording frame");
	}

	void RecordHead(std::uint64_t frame)
	{
		PoseSample head;
		head.pose.position = {static_cast<float>(frame), 1.7f, -2};
		CommandTransaction transaction{frame,
									   source,
									   {FrameId{frame}, {}},
									   CommandLatePolicy::Reject,
									   {TakeControl{ControlChannel::Head}, SetHeadPose{head}}};
		if (frame == 1)
		{
			ControllerControls held;
			held.trigger = .75f;
			transaction.commands.push_back(TakeControl{ControlChannel::LeftControls});
			transaction.commands.push_back(SetControllerControls{0, held});
		}
		Require(session->Commands().Submit(transaction).result == CommandResult::Queued, "Queue head edit");
		Frame();
	}

	Runtime runtime{ExtensionRegistry{std::span<const ExtensionDefinition>{}}};
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	wire::ControlService service{runtime};
	std::unique_ptr<wire::LocalControlServer> server;
	std::filesystem::path path;
	const CommandSource source{CommandSourceKind::Manual, 991};
};

app::RecordingSnapshot Finished(app::RecordingController& controller)
{
	Eventually([&] { return !controller.Snapshot().busy; }, "Recording worker did not finish");
	return controller.Snapshot();
}

app::RecordingSnapshot Request(app::RecordingController& controller, app::RecordingAction action, std::string path = {})
{
	Require(controller.Request(action, 0, std::move(path)), "Recording action not admitted");
	return Finished(controller);
}

std::string Contents(const std::filesystem::path& path)
{
	std::ifstream input(path, std::ios::binary);
	Require(input.good(), "Read saved rig");
	return {std::istreambuf_iterator<char>(input), std::istreambuf_iterator<char>()};
}
#ifdef SIMXR_TEST_AUTOMATION
void Replay(RecordingHost& host, const std::string& script, std::uint64_t seed, std::uint64_t expected = 22,
			bool expectBody = false, const std::filesystem::path& archive = {}, bool checkControls = true)
{
	host.session->ReleaseCommandSource(host.source);
	app::WorkerConnection connection(host.server->Path(), "recorded_rig_test",
									 static_cast<std::uint64_t>(wire::Capability::SimulationCommands));
	automation::RigRunner runner(
		[&](auto opcode, auto fields) { return connection.Exchange(opcode, std::move(fields)); });
	automation::RigOptions options;
	options.session = wire::ControlSessionId{1};
	options.generation = host.session->Snapshot().generation.value;
	options.seed = seed;
	options.source = script;
	options.recordingArchive = archive;
	options.deadline = std::chrono::seconds{30};
	std::atomic<bool> stopping{false}, failed{false}, heldFrame{false}, releasedFrame{false}, bodyFrame{false};
	std::thread application([&] {
		while (!stopping)
		{
			FramePrediction prediction;
			auto frameResult = host.session->WaitFrame(prediction);
			if (frameResult == XR_SUCCESS)
				frameResult = host.session->BeginFrame();
			if (frameResult == XR_SUCCESS)
				frameResult = host.session->EndFrame(prediction);
			if (frameResult != XR_SUCCESS)
			{
				if (!stopping)
					failed = true;
				return;
			}
			const auto applied = host.session->Commands().At(prediction.id);
			if (applied && applied->configuration.tracking.bodyObservation)
			{
				const auto& body = *applied->configuration.tracking.bodyObservation;
				if (body.jointSet != BodyJointSet::FullBodyMETA || !body.returnedTime ||
					body.returnedTime->value != applied->time.value - Duration{20})
					failed = true;
				bodyFrame = true;
			}
			if (applied && applied->configuration.tracking.head.pose.position.x == 7)
				heldFrame = applied->configuration.input.controllers[0].trigger == .75f;
			if (applied && applied->configuration.tracking.head.pose.position.x == 8)
				releasedFrame = applied->configuration.input.controllers[0].trigger == 0;
			std::this_thread::sleep_for(std::chrono::milliseconds{1});
		}
	});
	try
	{
		runner.Start(std::move(options));
		runner.Join();
	}
	catch (...)
	{
		stopping = true;
		host.session->Close();
		application.join();
		throw;
	}
	stopping = true;
	host.session->Close();
	application.join();
	const auto result = runner.Snapshot();
	Require(!failed && result.phase == automation::RigPhase::Complete, result.error.c_str());
	Require(result.transactions == expected && (!checkControls || (heldFrame && releasedFrame == archive.empty())) &&
				bodyFrame == expectBody && result.checkpoints.size() == 1 && result.checkpoints.front().passed,
			"Complete saved rig did not execute every recorded transaction");
}
#endif
void SaveAndReplay(bool dense = false)
{
	RecordingHost host;
	app::RecordingController controller;
	Require(!controller.Request(app::RecordingAction::Start), "Recorder admitted an absent target");
	controller.SetTarget(host.Target());
	const auto started = Request(controller, app::RecordingAction::Start);
	Require(started.error.empty() && started.page && started.page->active, "Start recording failed");
	const auto destination = host.path / u8"editable 日本語 rig.luau";
	{
		std::ofstream sentinel(destination);
		sentinel << "keep existing destination";
	}
	const auto activeSave = Request(controller, app::RecordingAction::Save, app::PathText(destination));
	Require(!activeSave.error.empty() && Contents(destination) == "keep existing destination" &&
				host.session->Commands().Recorder().Snapshot().active,
			"Active save modified destination or disconnected recording owner");
	for (std::uint64_t frame = 1; frame <= 20; ++frame)
	{
		host.RecordHead(frame);
		if (frame == 7)
			host.session->ReleaseCommandSource(host.source);
	}
	const auto stopped = Request(controller, app::RecordingAction::Stop);
	Require(stopped.error.empty() && stopped.page && !stopped.page->active && stopped.page->total == 21,
			"Stop recording lost acknowledged transactions");
	const auto badDestination =
		Request(controller, app::RecordingAction::Save, app::PathText(host.path / "missing" / "rig"));
	Require(!badDestination.error.empty() && badDestination.savedPath.empty(), "Failed write reported a saved file");
	const auto truncatedPath = app::PathText(destination) + std::string("\0unexpected", 11);
	bool rejected = false;
	try
	{
		controller.Request(app::RecordingAction::Save, 0, truncatedPath);
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected && !controller.Snapshot().busy && Contents(destination) == "keep existing destination",
			"Embedded NUL must not redirect a recording save to a truncated path");
	const auto saved = Request(controller, app::RecordingAction::Save, app::PathText(destination));
	Require(saved.error.empty() && saved.savedPath == app::PathText(destination) && saved.page &&
				saved.page->transactions.size() == 21,
			"Save did not collect beyond its first page");
	bool rejectedOpen = false;
	try
	{
		(void)app::OpenSavedRecording(truncatedPath);
	}
	catch (const std::invalid_argument&)
	{
		rejectedOpen = true;
	}
	Require(rejectedOpen, "Embedded NUL must not reopen a different recording");
	const auto replacement = Request(controller, app::RecordingAction::Save, app::PathText(destination));
	Require(replacement.error.empty(), "Recording must replace an existing saved file");
	const auto failedDirectory = host.path / "failed-save";
	std::filesystem::create_directory(failedDirectory);
	const auto rejectedSave = Request(controller, app::RecordingAction::Save, app::PathText(failedDirectory));
	Require(!rejectedSave.error.empty() && rejectedSave.savedPath == saved.savedPath &&
				std::filesystem::is_empty(failedDirectory),
			"Failed save preserves the last successful recording");
	for (const auto& entry : std::filesystem::directory_iterator(host.path))
		Require(entry.path() == destination || entry.path() == failedDirectory ||
					entry.path().filename() == "control.sock",
				"Failed save left a temporary recording file");
	const auto script = Contents(destination);
	Require(script.find("recorded frame +20") != std::string::npos &&
				script.find("position = {20,") != std::string::npos,
			"Saved rig omitted its last page or editable values");
	std::uint64_t expected = 22;
	if (dense)
	{
		auto recording = *saved.page;
		recording.version = 4;
		BodyObservation body;
		body.jointSet = BodyJointSet::FullBodyMETA;
		body.located = true;
		body.active = true;
		body.confidence = .75f;
		body.skeletonRevision = 0;
		body.skeleton.emplace();
		for (const auto joint : BodyModel::JointMap(body.jointSet))
		{
			const auto index = static_cast<std::size_t>(joint);
			body.joints[index] = ObservedBodyJoint{IdentityPose(), ValidPoseFlags};
			(*body.skeleton)[index] = SkeletonJoint{joint, {}, IdentityPose()};
		}
		for (std::uint64_t frame = 21; frame <= 148; ++frame)
		{
			const RuntimeTime anchor{Duration{static_cast<std::int64_t>(1000000 + frame * 1000)}};
			body.requestedTime = anchor;
			body.returnedTime = RuntimeTime{anchor.value - Duration{20}};
			body.joints[0]->pose.position.x = static_cast<float>(frame);
			automation::RecordedTransaction transaction;
			transaction.id = frame;
			transaction.phase = "applied";
			transaction.frame = FrameId{frame};
			transaction.runtimeTime = anchor;
			transaction.commands = {TakeControl{ControlChannel::Body}, SetBodyControls{{}, body}};
			recording.transactions.push_back(std::move(transaction));
		}
		recording.total = recording.transactions.size();
		expected = recording.total + 1;
		app::SaveRecording(destination, recording, true);
		Require(std::filesystem::file_size(destination) > wire::MaxPayload,
				"Dense body take did not exercise a source larger than one IPC message");
	}
#ifdef SIMXR_TEST_AUTOMATION
	const auto reopened = app::OpenSavedRecording(app::PathText(destination));
	Require(reopened.seed == saved.savedSeed && reopened.path == app::PathText(destination),
			"Reopening lost recording metadata");
	Replay(host, Contents(app::Utf8Path(reopened.path)), reopened.seed, expected, dense);
#endif
}

void SavedRecordingBounds()
{
	RecordingHost host;
	const auto path = host.path / "metadata.luau";
	for (unsigned version = 1; version <= 5; ++version)
	{
		{
			std::ofstream output(path);
			output << "-- OpenXR Simulator recorded inputs, schema " << version
				   << ".\n-- Launch simxr_runner with --seed 73\n";
		}
		bool accepted = false;
		try
		{
			accepted = app::OpenSavedRecording(app::PathText(path)).seed == 73;
		}
		catch (const std::runtime_error&)
		{}
		Require(accepted == (version <= 4),
				"Saved-take schema recognition disagrees with the supported exporter schemas");
	}
	std::filesystem::resize_file(path, automation::MaxRigSourceBytes + 1);
	bool rejected = false;
	try
	{
		app::OpenSavedRecording(app::PathText(path));
	}
	catch (const std::runtime_error& error)
	{
		rejected = std::string(error.what()).find("64 MiB") != std::string::npos;
	}
	Require(rejected, "Oversized saved source bypassed the file budget");
}

app::RecordingCaptureSnapshot CaptureFinished(app::RecordingCapture& capture)
{
	Eventually([&] { return !capture.Snapshot().busy; }, "Capture worker did not finish");
	return capture.Snapshot();
}

void SustainedCapture()
{
	RecordingHost host;
	const auto game = host.runtime.Game().Register(SIMXR_GAME_CAN_RESET);
	Require(game && host.runtime.Game().Publish(game, "checkpoint:start"), "Publish SDK capture origin");
	app::RecordingCapture capture;
	Require(!capture.Start(host.path / "unbound"), "Capture admitted an absent target");
	capture.SetTarget(host.Target());
	const auto destination = host.path / "recordings" / u8"take 日本語.simxrtake";
	Require(capture.Start(destination), "Start sustained capture");
	Eventually([&] { return capture.Snapshot().phase != app::RecordingCapturePhase::Starting; },
			   "Capture did not start");
	Require(capture.Snapshot().phase == app::RecordingCapturePhase::Capturing, capture.Snapshot().error.c_str());
	Require(!capture.Start(host.path / "overlap"), "Concurrent take was admitted");
	for (std::uint64_t frame = 1; frame <= 1281; ++frame)
	{
		host.RecordHead(frame);
		Require(host.runtime.Game().Publish(game, "frame:" + std::to_string(frame)),
				"Publish captured SDK observation");
		if (frame % 64 == 0)
			Eventually([&] { return capture.Snapshot().transactions >= frame; }, "Capture did not drain live pages");
	}
	capture.Stop();
	const auto saved = CaptureFinished(capture);
	Require(saved.phase == app::RecordingCapturePhase::Complete && saved.transactions == 1281 && saved.error.empty(),
			"Sustained capture was not complete");
	automation::RecordingArchiveReader reader(destination);
	std::uint64_t count = 0;
	while (const auto page = reader.Next())
	{
		for (const auto& transaction : page->recording.transactions)
		{
			++count;
			const auto head = std::get_if<SetHeadPose>(&transaction.commands[1]);
			Require(head && head->value.pose.position.x == static_cast<float>(count) && transaction.runtimeTime,
					"Archive lost exact ordered head samples or runtime anchors");
		}
	}
	Require(count == 1281 && reader.State().complete && reader.State().identity == saved.identity,
			"Capture archive lost its footer or identity");
	Require(host.session->Commands().Recorder().Snapshot().floor == count, "Flushed pages were not acknowledged");
	automation::RecordingArchiveReader evidence(destination);
	Require(evidence.State().game && evidence.State().game->origin.journal.state.telemetry == "checkpoint:start",
			"Archive lost the SDK state at capture start");
	std::uint64_t observations = 0;
	while (const auto event = evidence.NextEvent())
		if (const auto* page = std::get_if<automation::RecordedGamePage>(&*event))
			for (const auto& observation : page->journal.observations)
			{
				Require(observation.game == game && observation.time &&
							observation.payload == "frame:" + std::to_string(++observations),
						"SDK capture lost ordered telemetry, identity or native timestamps");
			}
	Require(observations == count && evidence.State().complete && evidence.State().game->complete &&
				!evidence.State().game->gap && evidence.State().game->uncorrelated,
			"SDK archive lost completion or pretended deterministic time has wall-clock correlation");

	Require(capture.Start(destination), "Admit occupied-path validation");
	Require(CaptureFinished(capture).phase == app::RecordingCapturePhase::Incomplete &&
				host.session->Commands().Recorder().Snapshot().identity == saved.identity,
			"Occupied destination restarted recording or was reported complete");
	Require(capture.Start(host.path / "empty"), "Restart capture");
	capture.Stop();
	Require(CaptureFinished(capture).phase == app::RecordingCapturePhase::Complete,
			"Immediate stop failed to close an empty take");
	app::LibraryCatalog catalog(host.path);
	const auto catalogResult = [&] {
		std::optional<app::LibraryResult> result;
		Eventually(
			[&] {
				result = catalog.TakeResult();
				return result.has_value();
			},
			"Archive catalog did not finish");
		return std::move(*result);
	};
	Require(catalog.ReadEvidence(app::PathText(destination)), "Queue SDK library evidence");
	const auto inspected = catalogResult();
	Require(!inspected.error && inspected.evidence && inspected.evidence->entries.size() == 16 &&
				inspected.evidence->more && !inspected.evidence->correlated,
			"Library worker lost bounded SDK evidence or invented deterministic clock correlation");
	catalog.Refresh();
	auto listed = catalogResult();
	Require(!listed.error && listed.entries && listed.entries->size() == 1 && listed.entries->front().archive &&
				listed.entries->front().take && listed.entries->front().error.empty(),
			"Archive was not listed as a complete take");
	Require(catalog.Rename(app::PathText(destination), "renamed 日本語"), "Rename archive");
	const auto renamed = catalogResult();
	Require(renamed.changed && !renamed.error, "Archive rename failed");
	Require(catalog.Rename(renamed.renamed, "take 日本語"), "Restore archive name");
	Require(!catalogResult().error, "Restore archive failed");
	const auto incomplete = destination.parent_path() / "incomplete.simxrtake";
	std::filesystem::create_directory(incomplete);
	std::filesystem::copy_file(destination / "input.simxr", incomplete / "input.simxr");
	std::filesystem::resize_file(incomplete / "input.simxr",
								 std::filesystem::file_size(incomplete / "input.simxr") - 1);
	catalog.Refresh();
	const auto partial = catalogResult();
	Require(!partial.error && partial.entries && partial.entries->size() == 2 &&
				std::ranges::any_of(*partial.entries,
									[&](const auto& entry) {
										return entry.path == app::PathText(incomplete) && entry.archive &&
											   !entry.error.empty();
									}),
			"Incomplete archive was omitted or presented as complete");
	Require(catalog.Delete(app::PathText(incomplete)), "Delete incomplete archive");
	Require(!catalogResult().error, "Incomplete archive deletion failed");
	{
		std::ofstream extra(destination / "notes.txt");
		extra << "keep notes";
	}
	Require(catalog.Delete(app::PathText(destination)), "Admit protected archive delete");
	Require(catalogResult().error && std::filesystem::exists(destination / "input.simxr"),
			"Archive deletion removed unrelated files or input before checking contents");
	std::filesystem::remove(destination / "notes.txt");
#ifdef SIMXR_TEST_AUTOMATION
	const auto reopened = app::OpenSavedRecording(app::PathText(destination));
	Require(reopened.seed == reader.State().origin.seed && reopened.path == app::PathText(destination),
			"Archive metadata did not reopen through the companion loader");
	Replay(host, {}, reopened.seed, 1282, false, destination);
#endif
	const auto range = automation::ReadRecordingRange(destination, FrameId{1200}, FrameId{1281});
	Require(catalog.ExportRange(app::PathText(destination), "editable range 日本語", 1200, 1281),
			"Admit archive range export");
	const auto exported = catalogResult();
	Require(exported.changed && !exported.error &&
				Contents(app::Utf8Path(exported.renamed)) == automation::EditableRig(range),
			"Archive range worker did not save the exact editable rig");
	Require(catalog.ExportRange(app::PathText(destination), "editable range 日本語", 1200, 1281),
			"Admit conflicting range export");
	const auto conflict = catalogResult();
	Require(conflict.error && !conflict.changed &&
				Contents(app::Utf8Path(exported.renamed)) == automation::EditableRig(range),
			"Archive range export overwrote an existing file");
	Require(range.originFrame == FrameId{1200} && range.originRuntimeTime && range.total == 81 &&
				std::get<SetHeadPose>(range.initial.front()).value.pose.position.x == 1200,
			"Range did not reconstruct its boundary or trim the prefix");
	bool rejectedWide = false;
	try
	{
		(void)automation::ReadRecordingRange(destination, FrameId{1}, FrameId{1281});
	}
	catch (const std::length_error&)
	{
		rejectedWide = true;
	}
	Require(rejectedWide, "Oversized editable range ignored its bounded source contract");
	std::stop_source cancelled;
	cancelled.request_stop();
	bool rejectedCancel = false;
	try
	{
		(void)automation::ReadRecordingRange(destination, FrameId{1200}, FrameId{1281}, cancelled.get_token());
	}
	catch (const std::runtime_error&)
	{
		rejectedCancel = true;
	}
	Require(rejectedCancel, "Cancelled range export continued scanning");
#ifdef SIMXR_TEST_AUTOMATION
	RecordingHost trimmedHost;
	const auto script = automation::EditableRig(range);
	Replay(trimmedHost, script, range.seed, 82, false, {}, false);
#endif
	Require(catalog.Delete(app::PathText(destination)), "Delete archive");
	const auto deleted = catalogResult();
	Require(!deleted.error && deleted.changed && !std::filesystem::exists(destination), "Archive delete failed");
}
#ifdef SIMXR_TEST_AUTOMATION
void RejectArchive(RecordingHost& host, const std::filesystem::path& directory)
{
	app::WorkerConnection connection(host.server->Path(), "archive_preflight_test",
									 static_cast<std::uint64_t>(wire::Capability::SimulationCommands));
	std::uint64_t mutations = 0;
	automation::RigRunner runner([&](auto opcode, auto fields) {
		if (opcode != wire::OpCode::Status)
			++mutations;
		return connection.Exchange(opcode, std::move(fields));
	});
	automation::RigOptions options;
	options.session = wire::ControlSessionId{1};
	options.generation = host.Target().generation;
	options.recordingArchive = directory;
	runner.Start(std::move(options));
	runner.Join();
	const auto result = runner.Snapshot();
	Require(result.phase == automation::RigPhase::Failed && result.transactions == 0 &&
				result.error.find("complete recording archive") != std::string::npos && mutations <= 1,
			"Incomplete archive reached replay mutation");
}
#endif
void CaptureSdkClock()
{
	RecordingHost host(ExecutionMode::RealTime);
	host.RecordHead(1);
	const auto game = host.runtime.Game().Register(SIMXR_GAME_CAN_RESET);
	app::RecordingCapture capture;
	capture.SetTarget(host.Target());
	const auto path = host.path / "clock";
	Require(capture.Start(path), "Start clock-correlated capture");
	Eventually([&] { return capture.Snapshot().phase != app::RecordingCapturePhase::Starting; },
			   "Clock capture did not start");
	Require(capture.Snapshot().phase == app::RecordingCapturePhase::Capturing, capture.Snapshot().error.c_str());
	host.RecordHead(2);
	std::this_thread::sleep_for(std::chrono::milliseconds{25});
	Require(host.runtime.Game().Publish(game, "after input"), "Publish correlated SDK observation");
	host.RecordHead(3);
	capture.Stop();
	const auto saved = CaptureFinished(capture);
	Require(saved.phase == app::RecordingCapturePhase::Complete, saved.error.c_str());
	automation::RecordingArchiveReader reader(path);
	Require(reader.State().game && reader.State().game->origin.clock &&
				reader.State().game->origin.clock->generation == host.session->Snapshot().generation,
			"Real-time capture omitted its verified session clock mapping");
	bool observed = false;
	while (const auto event = reader.NextEvent())
		if (const auto* page = std::get_if<automation::RecordedGamePage>(&*event))
			for (const auto& observation : page->journal.observations)
			{
				Require(page->clock && observation.time && page->clock->domain == observation.time->domain &&
							page->clock->runtimeBefore <= page->clock->runtimeAfter,
						"Captured SDK clock domain or uncertainty changed");
				observed |= observation.payload == "after input";
			}
	Require(observed && reader.State().complete && !reader.State().game->uncorrelated,
			"SDK real-time evidence was missing or uncorrelated");
	const auto evidence = automation::ReadRecordingEvidence(path);
	Require(evidence.entries.size() == 1 && evidence.entries[0].first && evidence.entries[0].last,
			"Real-time SDK observation did not resolve surrounding input boundaries");
	const auto range = automation::ReadRecordingRange(path, *evidence.entries[0].first, *evidence.entries[0].last);
	Require(range.originFrame == *evidence.entries[0].first,
			"SDK-selected range did not export from its observed input boundary");
}

void CaptureSameFrameReplay()
{
	RecordingHost host;
	app::RecordingCapture capture;
	capture.SetTarget(host.Target());
	const auto destination = host.path / "same-frame";
	Require(capture.Start(destination), "Start same-frame capture");
	Eventually([&] { return capture.Snapshot().phase == app::RecordingCapturePhase::Capturing; },
			   "Capture did not start");
	for (std::uint64_t index = 1; index <= 17; ++index)
	{
		PoseSample head;
		head.pose.position = {static_cast<float>(index), 1.7f, -2};
		CommandTransaction transaction{index,
									   host.source,
									   {FrameId{1}, {}},
									   CommandLatePolicy::Reject,
									   {TakeControl{ControlChannel::Head}, SetHeadPose{head}}};
		Require(host.session->Commands().Submit(transaction).result == CommandResult::Queued,
				"Queue same-frame transaction");
	}
	host.Frame();
	host.session->ReleaseCommandSource(host.source);
	PoseSample head;
	head.pose.position = {18, 1.7f, -2};
	CommandTransaction next{18,
							host.source,
							{FrameId{2}, {}},
							CommandLatePolicy::Reject,
							{TakeControl{ControlChannel::Head}, SetHeadPose{head}}};
	Require(host.session->Commands().Submit(next).result == CommandResult::Queued, "Queue after release");
	host.Frame();
	capture.Stop();
	Require(CaptureFinished(capture).phase == app::RecordingCapturePhase::Complete, "Save same-frame capture");
	const auto afterRelease = automation::ReadRecordingRange(destination, FrameId{2}, FrameId{2});
	Require(afterRelease.total == 0 && std::get<SetHeadPose>(afterRelease.initial.front()).value.pose.position.x == 18,
			"Trim did not restore release fallback before applying the selected boundary");
	const auto full = automation::ReadRecordingRange(destination, FrameId{0}, FrameId{2});
	Require(full.originFrame == FrameId{0} && full.total == 19, "Range rejected the valid zero-frame origin");
	const auto trimmed = automation::ReadRecordingRange(destination, FrameId{1}, FrameId{2});
	Require(trimmed.total == 2 && std::get<SetHeadPose>(trimmed.initial.front()).value.pose.position.x == 17 &&
				trimmed.transactions.front().phase == "released" && trimmed.transactions.front().afterFrame,
			"Same-frame range boundary lost its final value or following release");
	(void)automation::EditableRig(trimmed);
#ifdef SIMXR_TEST_AUTOMATION
	Replay(host, {}, app::OpenSavedRecording(app::PathText(destination)).seed, 20, false, destination, false);
#endif
}

void CaptureBodyRange()
{
	RecordingHost host;
	app::RecordingCapture capture;
	capture.SetTarget(host.Target());
	const auto directory = host.path / "body-range.simxrtake";
	Require(capture.Start(directory), "Start body range capture");
	Eventually([&] { return capture.Snapshot().phase == app::RecordingCapturePhase::Capturing; },
			   "Body capture did not start");
	BodyObservation body;
	body.jointSet = BodyJointSet::FullBodyMETA;
	body.located = true;
	body.active = true;
	body.confidence = .75f;
	body.skeletonRevision = 7;
	body.requestedTime = RuntimeTime{Duration{1000000}};
	body.returnedTime = RuntimeTime{Duration{999980}};
	body.skeleton.emplace();
	for (const auto joint : BodyModel::JointMap(body.jointSet))
	{
		const auto index = static_cast<std::size_t>(joint);
		body.joints[index] = ObservedBodyJoint{IdentityPose(), ValidPoseFlags};
		(*body.skeleton)[index] = SkeletonJoint{joint, {}, IdentityPose()};
	}
	CommandTransaction transaction{1,
								   host.source,
								   {FrameId{1}, {}},
								   CommandLatePolicy::Reject,
								   {TakeControl{ControlChannel::Body}, SetBodyControls{{}, body}}};
	Require(host.session->Commands().Submit(transaction).result == CommandResult::Queued, "Queue range body");
	host.Frame();
	host.RecordHead(2);
	const auto boundary = host.session->Commands().At(FrameId{2});
	Require(boundary != nullptr, "Body range boundary missing");
	capture.Stop();
	Require(CaptureFinished(capture).phase == app::RecordingCapturePhase::Complete, "Body range capture failed");
	const auto range = automation::ReadRecordingRange(directory, FrameId{2}, FrameId{2});
	Require(range.version == 4 && range.originRuntimeTime == boundary->time && range.total == 0,
			"Body range lost schema or exact boundary anchor");
	bool found = false;
	for (const auto& command : automation::ReplayInitialCommands(range))
		if (const auto* value = std::get_if<SetBodyControls>(&command))
		{
			found = value->observation && value->observationOrigin == boundary->time &&
					value->observation->returnedTime == body.returnedTime &&
					value->observation->requestedTime == body.requestedTime &&
					value->observation->skeletonRevision == 7 && value->observation->skeleton.has_value();
		}
	Require(found, "Trim rewrote body sample time or discarded its replay anchor/rest skeleton");
	(void)automation::EditableRig(range);
}

void CaptureRetirement()
{
	RecordingHost host;
	const auto destination = host.path / "retired";
	{
		app::RecordingCapture capture;
		capture.SetTarget(host.Target());
		Require(capture.Start(destination), "Start retirement capture");
		Eventually([&] { return capture.Snapshot().phase == app::RecordingCapturePhase::Capturing; },
				   "Capture did not start");
		host.RecordHead(1);
		Eventually([&] { return capture.Snapshot().transactions == 1; }, "Capture did not persist retirement prefix");
		capture.SetTarget({});
		Require(CaptureFinished(capture).phase == app::RecordingCapturePhase::Incomplete,
				"Retirement reported a complete take");
		Eventually([&] { return !host.session->Commands().Recorder().Snapshot().active; },
				   "Retirement left recording active");
		automation::RecordingArchiveReader reader(destination);
		while (reader.Next())
		{}
		Require(!reader.State().complete && reader.State().next == 1, "Retirement prefix was not recoverable");
#ifdef SIMXR_TEST_AUTOMATION
		RejectArchive(host, destination);
#endif
		capture.SetTarget(host.Target());
		Require(capture.Start(host.path / "destroyed"), "Rebind capture");
		Eventually([&] { return capture.Snapshot().phase == app::RecordingCapturePhase::Capturing; },
				   "Rebound capture did not start");
	}
	Eventually([&] { return !host.session->Commands().Recorder().Snapshot().active; },
			   "Capture destruction left recording active");
}
#ifndef _WIN32
void CaptureWriteFailure()
{
	const auto child = ::fork();
	Require(child >= 0, "Fork disk failure fixture");
	if (!child)
	{
		try
		{
			RecordingHost host;
			app::RecordingCapture capture;
			capture.SetTarget(host.Target());
			const auto destination = host.path / "disk-failure";
			Require(capture.Start(destination), "Start disk failure capture");
			Eventually([&] { return capture.Snapshot().phase == app::RecordingCapturePhase::Capturing; },
					   "Capture did not start");
			const auto length = std::filesystem::file_size(destination / "input.simxr");
			rlimit limit{};
			Require(::getrlimit(RLIMIT_FSIZE, &limit) == 0, "Read file limit");
			limit.rlim_cur = length + 1;
			Require(std::signal(SIGXFSZ, SIG_IGN) != SIG_ERR && ::setrlimit(RLIMIT_FSIZE, &limit) == 0,
					"Set write failure limit");
			host.RecordHead(1);
			const auto failed = CaptureFinished(capture);
			Require(failed.phase == app::RecordingCapturePhase::Incomplete && !failed.error.empty() &&
						failed.transactions == 0 && host.session->Commands().Recorder().Snapshot().floor == 0,
					"Failed disk flush was acknowledged or reported complete");
			automation::RecordingArchiveReader reader(destination);
			while (reader.Next())
			{}
			Require(!reader.State().complete && reader.State().truncated && reader.State().next == 0,
					"Failed disk write did not retain a recoverable origin");
		}
		catch (const std::exception& error)
		{
			std::cerr << "Disk failure fixture: " << error.what() << '\n';
			::_exit(1);
		}
		::_exit(0);
	}
	int status = 0;
	Require(::waitpid(child, &status, 0) == child && WIFEXITED(status) && WEXITSTATUS(status) == 0,
			"Disk failure child failed");
}
#endif
void CaptureOverflow()
{
	RecordingHost host;
	app::RecordingCapture capture;
	capture.SetTarget(host.Target());
	const auto destination = host.path / "overflow";
	Require(capture.Start(destination), "Start overflow capture");
	Eventually([&] { return capture.Snapshot().phase == app::RecordingCapturePhase::Capturing; },
			   "Capture did not start");
	host.RecordHead(1);
	host.session->Commands().Recorder().Fail();
	const auto failed = CaptureFinished(capture);
	Require(failed.phase == app::RecordingCapturePhase::Incomplete && failed.transactions == 1,
			"Runtime failure lost its tail or reported a complete take");
	automation::RecordingArchiveReader reader(destination);
	while (reader.Next())
	{}
	Require(reader.State().origin.overflow && !reader.State().complete && reader.State().next == 1,
			"Overflow state was not preserved in the archive");
}

void TargetRetirement()
{
	RecordingHost host;
	{
		app::RecordingController controller;
		controller.SetTarget(host.Target());
		Require(Request(controller, app::RecordingAction::Start).error.empty(), "Start retirement fixture");
		controller.SetTarget({});
		Eventually([&] { return !host.session->Commands().Recorder().Snapshot().active; },
				   "Changing target left an owned recorder active");
		Require(!controller.Snapshot().page && !controller.Request(app::RecordingAction::Save, 0, "unbound.luau"),
				"Old target state survived retirement");
		controller.SetTarget(host.Target());
		Require(Request(controller, app::RecordingAction::Start).error.empty(),
				"Rebind did not obtain recording rights");
	}
	Eventually([&] { return !host.session->Commands().Recorder().Snapshot().active; },
			   "Controller destruction left an owned recorder active");
}
} // namespace simxr::recording_tests

int main(int argc, char** argv)
{
	using namespace simxr::recording_tests;
	try
	{
		if (argc > 1)
		{
#ifndef _WIN32
			Require(argc == 2 && std::string_view(argv[1]) == "--write-failure", "Unknown recording test argument");
			CaptureWriteFailure();
			std::cout << "PASS disk flush failure leaves an unacknowledged recoverable prefix\n";
			return 0;
#else
			(void)argv;
			throw std::invalid_argument("Unknown recording test argument");
#endif
		}
		std::cout << "Checking editable saves and replay\n" << std::flush;
		SaveAndReplay();
		SaveAndReplay(true);
		SavedRecordingBounds();
		TargetRetirement();
		std::cout << "Checking sustained archive capture and replay\n" << std::flush;
		SustainedCapture();
		std::cout << "Checking retirement, SDK correlation and trimmed replay\n" << std::flush;
		CaptureRetirement();
		CaptureOverflow();
		CaptureSdkClock();
		CaptureSameFrameReplay();
		CaptureBodyRange();
		std::cout << "PASS recording worker, complete file save, actual CPU rig replay and target retirement\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
