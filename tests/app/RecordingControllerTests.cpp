#include "../TestSupport.h"
#include "app/RecordingController.h"
#include "app/WorkerConnection.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "runtime/Runtime.h"
#ifdef SIMXR_TEST_AUTOMATION
#include "automation/RigRunner.h"
#endif

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <iterator>
#include <thread>

namespace
{
using namespace simxr;
namespace wire = control;
namespace app = simxr::app;
class RecordingHost
{
  public:
	RecordingHost()
	{
		char directory[] = "/tmp/simxr-recording-XXXXXX";
		Require(::mkdtemp(directory), "Create recording test directory");
		path = directory;
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		std::strcpy(create.applicationInfo.applicationName, "CPU recording workflow");
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
		Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
					runtime.CreateSession(instance, session) == XR_SUCCESS &&
					session->ConfigureCapturePool(FramePool{8}, true) == XR_SUCCESS,
				"Create recording session");
		FrameSchedulerConfig config;
		config.mode = ExecutionMode::Deterministic;
		const XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(config.epoch) == XR_SUCCESS && session->Begin(begin, config) == XR_SUCCESS,
				"Begin recording session");
		server = std::make_unique<wire::LocalControlServer>(service, (path / "control.sock").string());
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
		const CommandTransaction transaction{frame,
											 source,
											 {FrameId{frame}, {}},
											 CommandLatePolicy::Reject,
											 {TakeControl{ControlChannel::Head}, SetHeadPose{head}}};
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
	Eventually(
		[&]
		{
			return !controller.Snapshot().busy;
		},
		"Recording worker did not finish");
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
void Replay(RecordingHost& host, const std::string& script, std::uint64_t seed)
{
	host.session->ReleaseCommandSource(host.source);
	app::WorkerConnection connection(host.server->Path(), "recorded_rig_test",
									 static_cast<std::uint64_t>(wire::Capability::SimulationCommands));
	automation::RigRunner runner(
		[&](auto opcode, auto fields)
		{
			return connection.Exchange(opcode, std::move(fields));
		});
	automation::RigOptions options;
	options.session = wire::ControlSessionId{1};
	options.generation = host.session->Snapshot().generation.value;
	options.seed = seed;
	options.source = script;
	options.deadline = std::chrono::seconds{5};
	std::atomic<bool> stopping{false}, failed{false};
	std::thread application(
		[&]
		{
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
	Require(result.transactions == 21 && result.checkpoints.size() == 1 && result.checkpoints.front().passed,
			"Complete saved rig did not execute every recorded transaction");
}
#endif
void SaveAndReplay()
{
	RecordingHost host;
	app::RecordingController controller;
	Require(!controller.Request(app::RecordingAction::Start), "Recorder admitted an absent target");
	controller.SetTarget(host.Target());
	const auto started = Request(controller, app::RecordingAction::Start);
	Require(started.error.empty() && started.page && started.page->active, "Start recording failed");
	const auto destination = host.path / "editable rig.luau";
	{
		std::ofstream sentinel(destination);
		sentinel << "keep existing destination";
	}
	const auto activeSave = Request(controller, app::RecordingAction::Save, destination.string());
	Require(!activeSave.error.empty() && Contents(destination) == "keep existing destination" &&
				host.session->Commands().Recorder().Snapshot().active,
			"Active save modified destination or disconnected recording owner");
	for (std::uint64_t frame = 1; frame <= 20; ++frame)
		host.RecordHead(frame);
	const auto stopped = Request(controller, app::RecordingAction::Stop);
	Require(stopped.error.empty() && stopped.page && !stopped.page->active && stopped.page->total == 20,
			"Stop recording lost acknowledged transactions");
	const auto badDestination =
		Request(controller, app::RecordingAction::Save, (host.path / "missing" / "rig").string());
	Require(!badDestination.error.empty() && badDestination.savedPath.empty(), "Failed write reported a saved file");
	const auto saved = Request(controller, app::RecordingAction::Save, destination.string());
	Require(saved.error.empty() && saved.savedPath == destination.string() && saved.page &&
				saved.page->transactions.size() == 20,
			"Save did not collect beyond its first page");
	const auto script = Contents(destination);
	Require(script.find("recorded frame +20") != std::string::npos &&
				script.find("position = {20,") != std::string::npos,
			"Saved rig omitted its last page or editable values");
#ifdef SIMXR_TEST_AUTOMATION
	// Reopen from disk as a fresh companion does; replay must recover its original seed.
	const auto reopened = app::OpenSavedRecording(destination.string());
	Require(reopened.seed == saved.savedSeed && reopened.path == destination.string(),
			"Reopening lost recording metadata");
	Replay(host, Contents(reopened.path), reopened.seed);
#endif
}
void TargetRetirement()
{
	RecordingHost host;
	{
		app::RecordingController controller;
		controller.SetTarget(host.Target());
		Require(Request(controller, app::RecordingAction::Start).error.empty(), "Start retirement fixture");
		controller.SetTarget({});
		Eventually(
			[&]
			{
				return !host.session->Commands().Recorder().Snapshot().active;
			},
			"Changing target left an owned recorder active");
		Require(!controller.Snapshot().page && !controller.Request(app::RecordingAction::Save, 0, "unbound.luau"),
				"Old target state survived retirement");
		controller.SetTarget(host.Target());
		Require(Request(controller, app::RecordingAction::Start).error.empty(),
				"Rebind did not obtain recording rights");
	}
	Eventually(
		[&]
		{
			return !host.session->Commands().Recorder().Snapshot().active;
		},
		"Controller destruction left an owned recorder active");
}
} // namespace
int main()
{
	try
	{
		SaveAndReplay();
		TargetRetirement();
		std::cout << "PASS recording worker, complete file save, actual CPU rig replay and target retirement\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
