#include "../TestSupport.h"
#include "automation/Recording.h"
#include "automation/RecordingArchive.h"
#include "automation/RecordingRange.h"
#include "automation/RigProgress.h"
#include "automation/RigReport.h"
#include "automation/RigRunner.h"
#include "control/ControlSchema.h"
#include "control/ExportReader.h"
#include "control/ControlService.h"
#include "control/InspectionDocument.h"
#include "control/SimulationControl.h"
#include "math/Math.h"
#include "runtime/Runtime.h"
#include "simulation/BodyModel.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <future>
#include <iostream>
#include <limits>
#include <locale>
#include <thread>

using namespace simxr;
namespace wire = control;

struct Fixture
{
	FramePool pool{8};
	Runtime runtime{ExtensionRegistry{std::span<const ExtensionDefinition>{}}};
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	wire::ControlService service{runtime};
	wire::ConnectionId connection;
	std::uint64_t request = 0;

	explicit Fixture(bool capture = false)
	{
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		std::strcpy(create.applicationInfo.applicationName, "Luau runner integration");
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
		Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
					runtime.CreateSession(instance, session) == XR_SUCCESS,
				"Fixture create");
		if (capture)
			Require(session->ConfigureCapturePool(pool) == XR_SUCCESS, "Configure replay capture pool");
		FrameSchedulerConfig config;
		config.mode = ExecutionMode::Deterministic;
		XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(config.epoch) == XR_SUCCESS && session->Begin(begin, config) == XR_SUCCESS,
				"Fixture begin");
		connection = *service.Connect();
		Exchange(wire::OpCode::Hello, {});
	}

	wire::Message Exchange(wire::OpCode opcode, std::vector<wire::Field> fields)
	{
		wire::Message message;
		message.requestId = wire::RequestId{++request};
		message.opcode = static_cast<std::uint16_t>(opcode);
		message.fields = std::move(fields);
		auto result = service.Handle(connection, message);
		if (opcode == wire::OpCode::SubmitSimulation && result.Find(wire::tag::Phase) &&
			result.Find(wire::tag::Phase)->AsText() == "queued")
		{
			FramePrediction prediction;
			Require(session->WaitFrame(prediction) == XR_SUCCESS && session->BeginFrame() == XR_SUCCESS &&
						session->EndFrame(prediction) == XR_SUCCESS,
					"Actual scheduled core frame");
		}
		return result;
	}

	automation::RigOptions Options(std::string source)
	{
		automation::RigOptions options;
		options.session = wire::ControlSessionId{1};
		options.generation = session->Snapshot().generation.value;
		options.seed = 73;
		options.source = std::move(source);
		options.name = "@test-rig.luau";
		options.deadline = std::chrono::seconds{3};
		return options;
	}
};

void CheckSharedSequenceRigCancellation()
{
	Fixture fixture(true);
	const auto fields = [&] {
		return std::vector{wire::Field::U64(wire::tag::Session, 1),
						   wire::Field::U64(wire::tag::Generation, fixture.session->Snapshot().generation.value),
						   wire::Field::U32(wire::tag::SourceKind, static_cast<std::uint32_t>(CommandSourceKind::Rig))};
	};
	const std::string program = R"({"frame":0,"head":{"x":2},"capture":true})";
	auto upload = fields();
	upload.push_back(wire::Field::U64(wire::tag::ProgramId, 1));
	upload.push_back(wire::Field::U64(wire::tag::Offset, 0));
	upload.push_back(wire::Field::U64(wire::tag::TotalBytes, program.size()));
	upload.push_back(wire::Field::Bytes(wire::tag::Bytes, std::as_bytes(std::span(program))));
	Require(fixture.Exchange(wire::OpCode::UploadSequence, std::move(upload)).Find(wire::tag::Error)->AsU32() == 0,
			"Stage a native sequence on the rig connection");
	auto start = fields();
	start.push_back(wire::Field::U64(wire::tag::ProgramId, 1));
	start.push_back(wire::Field::Bool(wire::tag::Loop, true));
	Require(fixture.Exchange(wire::OpCode::StartSequence, std::move(start)).Find(wire::tag::Error)->AsU32() == 0,
			"Start a native sequence on the rig connection");
	automation::RigRunner runner(
		[&](auto opcode, auto request) { return fixture.Exchange(opcode, std::move(request)); },
		[&] { fixture.service.Disconnect(fixture.connection); });
	runner.Start(fixture.Options("simxr.start_input_recording() while true do simxr.wait(10) end"));
	Eventually(
		[&] {
			const auto result = runner.Snapshot();
			Require(result.phase == automation::RigPhase::Running, result.error.c_str());
			return fixture.session->Commands().Recorder().Snapshot({}, 0).active;
		},
		"Rig recording did not begin");
	FramePrediction frame;
	Require(fixture.session->WaitFrame(frame) == XR_SUCCESS && fixture.session->BeginFrame() == XR_SUCCESS &&
				fixture.session->EndFrame(frame) == XR_SUCCESS &&
				fixture.session->Captures().Snapshot().captures.size() == 1,
			"Rig's native loop did not retain an exact capture");
	runner.Cancel();
	runner.Join();
	const CommandSource owner{CommandSourceKind::Rig, fixture.connection.value};
	Require(runner.Snapshot().phase == automation::RigPhase::Cancelled &&
				!fixture.session->Commands().PoseSequenceStatusFor(owner)->active &&
				!fixture.session->Commands().Recorder().Snapshot({}, 0).active &&
				fixture.session->Captures().Snapshot().captures.empty(),
			"Rig cancellation did not retire its native sequence, recording and captures together");
}

void CheckTransportRetirementAfterCleanupFailure()
{
	Fixture fixture;
	bool disconnected = false;
	automation::RigRunner runner(
		[&](auto opcode, auto fields) {
			if (opcode == wire::OpCode::ReleaseControl)
				throw std::runtime_error("Injected cleanup transport failure");
			return fixture.Exchange(opcode, std::move(fields));
		},
		[&] {
			fixture.service.Disconnect(fixture.connection);
			disconnected = true;
		});
	runner.Start(fixture.Options("simxr.start_input_recording()"));
	runner.Join();
	const auto status = runner.Snapshot();
	Require(status.phase == automation::RigPhase::Failed &&
				status.error.find("Injected cleanup transport failure") != std::string::npos && disconnected &&
				!fixture.session->Commands().Recorder().Snapshot({}, 0).active,
			"Rig reported completion before disconnect released recording ownership after cleanup failure");
	const CommandSource successor{CommandSourceKind::Mcp, 52};
	Require(fixture.session->StartCommandRecording(false, successor) == RecordingResult::Ok,
			"Failed rig transport retained native recording ownership");
	fixture.session->ReleaseCommandSource(successor, true);
}

void CheckControllerBindingRigOwnership()
{
	Fixture fixture;
	CommandTransaction transaction;
	transaction.id = 1;
	transaction.source = {CommandSourceKind::Manual, 71};
	transaction.target.frame = FrameId{1};
	transaction.commands = {TakeControl{ControlChannel::LeftControls}};
	Require(fixture.session->Commands().Submit(transaction).result == CommandResult::Queued,
			"Cannot acquire baseline controller ownership");
	FramePrediction prediction;
	Require(fixture.session->WaitFrame(prediction) == XR_SUCCESS && fixture.session->BeginFrame() == XR_SUCCESS &&
				fixture.session->EndFrame(prediction) == XR_SUCCESS,
			"Cannot publish owned input");
	std::ifstream file(std::filesystem::path{SIMXR_RIG_EXAMPLES_DIR} / "controller_bindings.luau");
	Require(file.is_open(), "Cannot open controller binding rig");
	std::string source{std::istreambuf_iterator<char>{file}, {}};
	automation::RigRunner runner([&](auto opcode, auto fields) { return fixture.Exchange(opcode, std::move(fields)); });
	runner.Start(fixture.Options(std::move(source)));
	runner.Join();
	const auto result = runner.Snapshot();
	Require(result.phase == automation::RigPhase::Failed && result.transactions == 0 &&
				result.error.find("Release existing input owners") != std::string::npos,
			"Controller binding rig did not reject a shared input session");
	Require(fixture.session->Commands().At(prediction.id)->owners[3] == transaction.source,
			"Rejected controller binding rig changed existing ownership");
}

void CheckSweepRigs()
{
	for (const auto* name : {"pose_sweep.luau", "wheel_sweep.luau"})
	{
		const bool wheel = std::string_view{name} == "wheel_sweep.luau";
		std::ifstream file(std::filesystem::path{SIMXR_RIG_EXAMPLES_DIR} / name);
		Require(file.is_open(), "Cannot open sweep rig");
		std::string source{std::istreambuf_iterator<char>{file}, {}};
		Require(!file.bad(), "Cannot read sweep rig");
		Fixture fixture(true);
		unsigned frames = 0;
		automation::RigRunner runner([&](auto opcode, auto fields) {
			wire::Message request;
			request.requestId = wire::RequestId{++fixture.request};
			request.opcode = static_cast<std::uint16_t>(opcode);
			request.fields = std::move(fields);
			auto result = fixture.service.Handle(fixture.connection, request);
			if (fixture.session->Scheduler().Snapshot().permits)
			{
				FramePrediction prediction;
				Require(fixture.session->WaitFrame(prediction) == XR_SUCCESS &&
							fixture.session->BeginFrame() == XR_SUCCESS,
						"Sweep frame admission failed");
				const auto applied = fixture.session->Commands().At(prediction.id);
				Require(applied && prediction.id.value == frames + 1, "Sweep skipped an application frame");
				const auto& tracking = applied->configuration.tracking;
				const auto orientation =
					wheel ? tracking.controllers[1].pose.pose.orientation : tracking.head.pose.orientation;
				const auto norm = orientation.x * orientation.x + orientation.y * orientation.y +
								  orientation.z * orientation.z + orientation.w * orientation.w;
				Require(std::abs(norm - 1) < .0001f, "Sweep quaternion is not normalized");
				Require(std::count_if(applied->owners.begin(), applied->owners.end(),
									  [](const auto& owner) { return owner.has_value(); }) == (wheel ? 4 : 1),
						"Sweep took unrelated input channels");
				if (wheel)
				{
					const auto left = tracking.controllers[0].pose.pose.position;
					const auto right = tracking.controllers[1].pose.pose.position;
					Require(std::abs(left.x + right.x) < .0001f && std::abs(left.y + right.y - 2.7f) < .0001f &&
								std::abs(left.z + right.z + .9f) < .0001f,
							"Wheel hands left opposite sides of the configured center");
					const auto x = right.x, y = right.y - 1.35f, z = right.z + .45f;
					Require(std::abs(x * x + y * y + z * z - .18f * .18f) < .0001f &&
								std::abs(y * std::sin(.34906585f) + z * std::cos(.34906585f)) < .0001f,
							"Wheel changed radius or left its tilted plane");
					for (const auto& controller : applied->configuration.input.controllers)
						Require(controller.grip == 1, "Wheel released grip during motion");
					if (frames == 30)
						Require(right.x > .03f && right.x < .032f && right.y < 1.19f && orientation.z > .6f,
								"Wheel did not reach its configured steering amplitude");
				}
				else
				{
					Require(tracking.head.pose.position.x == 0 && tracking.head.pose.position.y == 1.7f &&
								tracking.head.pose.position.z == 0,
							"Orientation sweep translated the head");
					if (frames == 30)
						Require(orientation.y > .25f && orientation.z < -.12f,
								"Orientation sweep did not rotate yaw and roll");
					if (frames == 15)
						Require(orientation.x > .14f, "Orientation sweep did not rotate pitch");
				}
				++frames;
				Require(fixture.session->EndFrame(prediction) == XR_SUCCESS, "Sweep frame completion failed");
			}
			return result;
		});
		auto options = fixture.Options(std::move(source));
		options.name = "@" + std::string(name);
		// The full 121-frame sweep checks scheduling, not host execution speed.
		options.deadline = std::chrono::seconds{30};
		runner.Start(std::move(options));
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Complete, runner.Snapshot().error.c_str());
		Require(frames == 121 && runner.Snapshot().transactions == frames, "Sweep cycle length changed");
		FramePrediction cleanup;
		Require(fixture.session->WaitFrame(cleanup) == XR_SUCCESS && fixture.session->BeginFrame() == XR_SUCCESS &&
					fixture.session->EndFrame(cleanup) == XR_SUCCESS,
				"Sweep cleanup frame failed");
		const auto applied = fixture.session->Commands().At(cleanup.id);
		Require(applied &&
					std::none_of(applied->owners.begin(), applied->owners.end(),
								 [](const auto& owner) { return owner.has_value(); }) &&
					applied->configuration.input.controllers[0].grip == 0 &&
					applied->configuration.input.controllers[1].grip == 0,
				"Sweep retained input after completion");
	}
}

void CheckBindingPauseAndStep()
{
	Fixture fixture;
	std::atomic<bool> stateEntered = false;
	std::atomic<bool> continueState = false;
	std::atomic<bool> intercepted = false;
	automation::RigRunner runner([&](auto opcode, auto fields) {
		if (opcode == wire::OpCode::SimulationStatus && !intercepted.exchange(true))
		{
			stateEntered = true;
			Eventually([&] { return continueState.load(); }, "State interception was not released");
		}
		return fixture.Exchange(opcode, std::move(fields));
	});
	runner.Start(fixture.Options("simxr.state()\nprint('one binding')\nsimxr.checkpoint('after resume', true)"));
	try
	{
		Eventually([&] { return stateEntered.load(); }, "Script did not enter state binding");
		runner.Pause();
		continueState = true;
		Eventually([&] { return runner.Snapshot().phase == automation::RigPhase::Paused; },
				   "Runner did not pause at binding boundary");
		runner.Step();
		Eventually([&] { return runner.Snapshot().output.size() == 1; }, "One binding permit did not run print");
		Require(runner.Snapshot().checkpoints.empty(), "One binding permit ran a second binding");
		runner.Resume();
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Complete && runner.Snapshot().checkpoints.size() == 1,
				"Resumed runner did not finish");
	}
	catch (...)
	{
		continueState = true;
		runner.Cancel();
		runner.Join();
		throw;
	}
}

void CheckResumeClearsSteps()
{
	automation::RigProgress progress;
	automation::RigOptions options;
	options.deadline = std::chrono::milliseconds{20};
	progress.Start(options);
	progress.Step();
	progress.Resume();
	progress.Pause();
	bool deadline = false;
	try
	{
		progress.Check(true);
	}
	catch (const std::runtime_error& failure)
	{
		deadline = std::string_view{failure.what()}.find("deadline") != std::string_view::npos;
	}
	Require(deadline, "Resume left a step permit that bypassed a later pause");
}

void CheckTimedWaits()
{
	for (unsigned mode = 0; mode < 3; ++mode)
	{
		automation::RigRunner runner([](auto opcode, const auto&) {
			Require(opcode == wire::OpCode::Status || opcode == wire::OpCode::ReleaseControl,
					"An observational wait must not poll, acquire control, or step the application");
			wire::Message response;
			response.fields.push_back(wire::Field::U32(wire::tag::Error, 0));
			return response;
		});
		automation::RigOptions options;
		options.session = wire::ControlSessionId{1};
		options.generation = 1;
		options.deadline = mode == 1 ? std::chrono::milliseconds{50} : std::chrono::milliseconds{3000};
		options.source = mode ? "print('waiting') simxr.wait(30000)" : "simxr.wait(40) simxr.wait(0)";
		const auto started = std::chrono::steady_clock::now();
		runner.Start(std::move(options));
		if (mode == 2)
		{
			Eventually([&] { return !runner.Snapshot().output.empty(); }, "Rig did not reach its wait");
			runner.Cancel();
		}
		auto joined = std::async(std::launch::async, [&] { runner.Join(); });
		Require(joined.wait_for(std::chrono::seconds{1}) == std::future_status::ready,
				"Cancellation or execution deadline did not interrupt the timed wait");
		joined.get();
		const auto result = runner.Snapshot();
		Require(result.phase == (mode == 2	 ? automation::RigPhase::Cancelled
								 : mode == 1 ? automation::RigPhase::Failed
											 : automation::RigPhase::Complete),
				"Timed wait ended in the wrong phase");
		if (!mode)
			Require(std::chrono::steady_clock::now() - started >= std::chrono::milliseconds{40},
					"Wait returned before its target");
		if (mode == 1)
			Require(result.error.find("deadline") != std::string::npos,
					"Timed wait failed for a reason other than its deadline");
	}
}

void CheckPausedCancellation()
{
	automation::RigProgress progress;
	automation::RigOptions options;
	options.deadline = std::chrono::seconds{5};
	progress.Start(options);
	progress.Pause();
	auto worker = std::async(std::launch::async, [&] {
		try
		{
			progress.Check(true);
			return false;
		}
		catch (const std::runtime_error& error)
		{
			return std::string_view(error.what()) == "Rig cancelled";
		}
	});
	Eventually([&] { return progress.Phase() == automation::RigPhase::Paused; }, "Progress did not pause");
	progress.Cancel();
	Require(worker.wait_for(std::chrono::seconds{1}) == std::future_status::ready && worker.get(),
			"Paused cancellation waited for the execution deadline");
}

void CheckSteppedExecutionPhase()
{
	Fixture fixture;
	std::atomic<bool> entered = false, release = false;
	automation::RigRunner runner([&](auto opcode, auto fields) {
		if (opcode == wire::OpCode::SimulationStatus && !entered.exchange(true))
			Eventually([&] { return release.load(); }, "State interception was not released");
		return fixture.Exchange(opcode, std::move(fields));
	});
	runner.Start(fixture.Options("simxr.state()\nsimxr.state()\nwhile true do end"));
	try
	{
		Eventually([&] { return entered.load(); }, "Rig did not enter its first binding");
		runner.Pause();
		release = true;
		Eventually([&] { return runner.Snapshot().phase == automation::RigPhase::Paused; },
				   "Rig did not reach a paused binding");
		runner.Step();
		Eventually([&] { return runner.Snapshot().phase == automation::RigPhase::Running; },
				   "Stepped Luau execution was reported as paused");
		runner.Cancel();
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Cancelled, "Stepped run did not cancel");
	}
	catch (...)
	{
		release = true;
		runner.Cancel();
		runner.Join();
		throw;
	}
}

void CheckCleanupFailures()
{
	for (const auto failed :
		 {wire::OpCode::ReleaseImageWindow, wire::OpCode::ReleaseInspection, wire::OpCode::Relinquish})
	{
		Fixture fixture;
		bool attempted = false, inputsReleased = false;
		automation::RigRunner runner([&](auto opcode, auto fields) {
			if (opcode == wire::OpCode::ReleaseControl)
				inputsReleased = true;
			if (opcode == failed)
			{
				attempted = true;
				wire::Message response;
				response.fields = {
					wire::Field::U32(wire::tag::Error, static_cast<std::uint32_t>(wire::ErrorCode::Internal)),
					wire::Field::Text(wire::tag::Detail, "injected cleanup failure")};
				return response;
			}
			if (opcode == wire::OpCode::BeginImageWindow || opcode == wire::OpCode::InspectFrame)
			{
				wire::Message response;
				response.fields = {wire::Field::U32(wire::tag::Error, 0),
								   wire::Field::U64(opcode == wire::OpCode::BeginImageWindow ? wire::tag::ImageWindowId
																							 : wire::tag::InspectionId,
													42),
								   wire::Field::Bytes(wire::tag::Document, wire::InspectionValue::Object({}).Encode())};
				return response;
			}
			return fixture.Exchange(opcode, std::move(fields));
		});
		runner.Start(fixture.Options(failed == wire::OpCode::ReleaseImageWindow	 ? "simxr.begin_image_window()"
									 : failed == wire::OpCode::ReleaseInspection ? "simxr.inspect('7')"
																				 : "simxr.pause()"));
		runner.Join();
		Require(attempted && inputsReleased && runner.Snapshot().phase == automation::RigPhase::Failed &&
					runner.Snapshot().error.find("injected cleanup failure") != std::string::npos,
				"Failed resource cleanup was hidden or prevented remaining cleanup");
	}
}

void CheckScriptValueBoundaries()
{
	Fixture fixture;
	unsigned submissions = 0;
	automation::RigRunner runner([&](auto opcode, auto fields) {
		if (opcode == wire::OpCode::SubmitSimulation)
			++submissions;
		if (opcode == wire::OpCode::SimulationStatus)
		{
			wire::Message response;
			const auto document = wire::EncodeFields(
				std::vector{wire::Field::Text(1, ""), wire::Field::U32(2, 999), wire::Field::Bytes(3, {})});
			response.fields = {wire::Field::U32(wire::tag::Error, 0),
							   wire::Field::Bytes(wire::tag::Document, document)};
			return response;
		}
		return fixture.Exchange(opcode, std::move(fields));
	});
	runner.Start(fixture.Options(R"(
for _, duration in {-1, 0.5, 3600001, math.huge} do assert(not pcall(simxr.wait, duration)) end
local ok, detail = pcall(simxr.state)
assert(not ok and string.find(detail, 'Unknown inspection value type'))
for _, anchor in {'4294967296', '18446744073709551615'} do
    assert(not pcall(function()
        simxr.schedule({frame='1', commands={{kind='tracker', anchor=anchor}}})
    end))
end
for _, value in {1e100, -1e100, math.huge, -math.huge, 0/0} do
    for _, command in {{kind='gaze', yaw=value}, {kind='head_pose', position={value, 0, 0}}} do
        local accepted, error = pcall(function()
            simxr.schedule({frame='1', commands={command}})
        end)
        assert(not accepted and string.find(error, 'finite float value'))
    end
end
)"));
	runner.Join();
	Require(runner.Snapshot().phase == automation::RigPhase::Complete && submissions == 0,
			"Malformed inspection, tracker anchor or nonrepresentable float escaped script validation");
}

void CheckReportText()
{
	const std::string replacement = "\xef\xbf\xbd";
	for (const std::string invalid : {"\xff", "\xc0\x80", "\xed\xa0\x80", "\xf4\x90\x80\x80", "\xe2\x82"})
	{
		automation::RigSnapshot snapshot;
		snapshot.phase = automation::RigPhase::Failed;
		snapshot.error = invalid;
		std::string expected;
		for (std::size_t index = 0; index < invalid.size(); ++index)
			expected += replacement;
		Require(automation::SerializeRigJson(snapshot).find("\"error\":\"" + expected + "\"") != std::string::npos &&
					automation::SerializeRigJunit(snapshot).find("message=\"" + expected + "\"") != std::string::npos,
				"Invalid script UTF-8 corrupted a report");
	}
	automation::RigSnapshot snapshot;
	snapshot.phase = automation::RigPhase::Failed;
	snapshot.error = "\xc3\xa9\xf0\x9f\x98\x80";
	Require(automation::SerializeRigJson(snapshot).find(snapshot.error) != std::string::npos &&
				automation::SerializeRigJunit(snapshot).find(snapshot.error) != std::string::npos,
			"Report damaged valid Unicode");
	snapshot.error = "\xef\xbf\xbf\r\n\t";
	Require(automation::SerializeRigJunit(snapshot).find(replacement + "&#13;&#10;&#9;") != std::string::npos,
			"JUnit contains a forbidden XML character or loses attribute whitespace");

	struct GroupedNumbers : std::numpunct<char>
	{
		std::string do_grouping() const override
		{
			return "\3";
		}
	};

	const auto previous = std::locale();
	std::locale::global(std::locale{previous, new GroupedNumbers});
	snapshot.transactions = 1234;
	const auto json = automation::SerializeRigJson(snapshot);
	std::locale::global(previous);
	Require(json.find("\"transactions\":1234") != std::string::npos, "Locale grouping corrupted JSON numbers");
}

void CheckInputRecording()
{
	Fixture fixture;
	automation::RigRunner runner([&](auto opcode, auto fields) { return fixture.Exchange(opcode, std::move(fields)); });
	runner.Start(fixture.Options(R"(
assert(simxr.start_input_recording().active)
local transaction = simxr.schedule({frame="1", commands={
    {kind="take_control", channel="head"}, {kind="head_pose", position={0.25, 1.7, 0}},
}})
simxr.await_transaction(transaction)
assert(not simxr.stop_input_recording().active)
local recording = simxr.read_input_recording("0", "16")
assert(recording.total == "1" and recording.transactions[1].phase == "applied")
assert(not pcall(function() simxr.read_input_recording(0, 17) end))
)"));
	runner.Join();
	Require(runner.Snapshot().phase == automation::RigPhase::Complete, runner.Snapshot().error.c_str());
	runner.Start(fixture.Options("simxr.start_input_recording() error('injected recording script failure')"));
	runner.Join();
	Require(runner.Snapshot().phase == automation::RigPhase::Failed &&
				!fixture.session->Commands().Recorder().Snapshot(0, 0).active,
			"Failed rig left input recording active");
	runner.Start(fixture.Options("simxr.start_input_recording() simxr.stop_input_recording()"));
	runner.Join();
	Require(runner.Snapshot().phase == automation::RigPhase::Complete, "Input recording ownership survived cleanup");
}

void CheckRecordedPlaytestOutcome()
{
	struct ArchiveDirectory
	{
		std::filesystem::path path =
			std::filesystem::temp_directory_path() /
			("simxr-sdk-playtest-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count()));

		ArchiveDirectory()
		{
			Require(std::filesystem::create_directory(path), "Create private playtest directory");
		}

		~ArchiveDirectory()
		{
			std::error_code ignored;
			std::filesystem::remove_all(path, ignored);
		}
	} directory;

	Fixture captured;
	std::unique_ptr<automation::RecordingArchiveWriter> archive;
	automation::RigRunner recorder([&](auto opcode, auto fields) {
		if (opcode == wire::OpCode::StartRecording)
			fields.push_back(wire::Field::Bool(wire::tag::RecordingDrain, true));
		if (opcode == wire::OpCode::StopRecording)
			fields.push_back(wire::Field::U64(wire::tag::RecordingId, archive->State().identity));
		auto result = captured.Exchange(opcode, std::move(fields));
		if (opcode == wire::OpCode::StartRecording)
			archive = std::make_unique<automation::RecordingArchiveWriter>(directory.path / "take", result);
		if (opcode == wire::OpCode::StopRecording)
		{
			auto fields =
				std::vector{wire::Field::U64(wire::tag::Session, 1),
							wire::Field::U64(wire::tag::Generation, captured.session->Snapshot().generation.value),
							wire::Field::U64(wire::tag::RecordingId, archive->State().identity),
							wire::Field::U64(wire::tag::Offset, 0), wire::Field::U64(wire::tag::Length, 16)};
			const auto page = captured.Exchange(wire::OpCode::ReadRecording, std::move(fields));
			Require(archive->Append(page) == 3, "SDK playtest archive lost captured input");
			archive->Finish(result);
		}
		return result;
	});
	BodyObservation recordedBody;
	recordedBody.jointSet = BodyJointSet::WithoutArmsBD;
	recordedBody.located = true;
	recordedBody.allJointPosesTracked = false;
	recordedBody.requestedTime = RuntimeTime{Duration{100}};
	for (const auto joint : BodyModel::JointMap(recordedBody.jointSet))
		recordedBody.joints[static_cast<std::size_t>(joint)] = ObservedBodyJoint{IdentityPose(), ValidPoseFlags};
	const auto bodyCommand = automation::EditableCommand(SetBodyControls{{}, recordedBody, RuntimeTime{Duration{120}}});
	auto captureScript = std::string{R"(
simxr.start_input_recording()
local origin = simxr.schedule({frame="1", commands={
    {kind="take_control", channel="head"},
    {kind="take_control", channel="left_controls"},
    {kind="take_control", channel="body"},
    {kind="head_pose", position={0.25, 1.7, 0}},
    BODY_COMMAND
}})
simxr.await_transaction(origin)
local pressed = simxr.schedule({frame="2", commands={
    {kind="controller_controls", hand=0, primary=true},
}})
simxr.await_transaction(pressed)
local released = simxr.schedule({frame="3", commands={
    {kind="controller_controls", hand=0, primary=false},
}})
simxr.await_transaction(released)
simxr.stop_input_recording()
)"};
	captureScript.replace(captureScript.find("BODY_COMMAND"), std::string_view("BODY_COMMAND").size(), bodyCommand);
	recorder.Start(captured.Options(std::move(captureScript)));
	recorder.Join();
	Require(recorder.Snapshot().phase == automation::RigPhase::Complete, recorder.Snapshot().error.c_str());
	Require(archive && archive->State().complete, "SDK playtest archive did not finish");
	archive.reset();
	auto original = automation::ReadRecordingRange(directory.path / "take", FrameId{1}, FrameId{3});
	Require(original.originFrame == FrameId{1} && original.transactions.size() == 2,
			"Trim did not fold the captured prefix into the editable rig origin");
	Require(original.originRuntimeTime.has_value(), "Trimmed application boundary has no runtime clock");

	for (unsigned mode = 0; mode < 3; ++mode)
	{
		const bool edit = mode == 1, rejectReset = mode == 2;
		auto recording = original;
		if (edit)
			for (auto& command : recording.initial)
				if (auto* head = std::get_if<SetHeadPose>(&command))
					head->value.pose.position.x = .75f;
		Fixture replay(true);
		SimxrGameBridgeApi api{};
		api.size = sizeof(api);
		Require(replay.runtime.Game().FillApi(1, api), "Replay game API unavailable");
		const auto game = api.registerGame(api.context, SIMXR_GAME_CAN_RESET);
		Require(game != 0, "Replay game registration failed");
		bool reset = false, hit = false, bodyVerified = false;
		unsigned presses = 0, advancedFrames = 0;
		automation::RigRunner runner([&](auto opcode, auto fields) {
			wire::Message request;
			request.requestId = wire::RequestId{++replay.request};
			request.opcode = static_cast<std::uint16_t>(opcode);
			request.fields = std::move(fields);
			auto result = replay.service.Handle(replay.connection, request);
			SimxrGameCommand command{};
			if (api.poll(api.context, game, &command))
			{
				Require(command.kind == SIMXR_GAME_RESET &&
							std::string_view(command.payload, command.payloadLength) == "checkpoint:target",
						"Replay reset the wrong game checkpoint");
				reset = !rejectReset;
				hit = false;
				presses = 0;
				Require(api.publish(api.context, game, "miss", 4) &&
							api.complete(api.context, game, command.id, rejectReset ? 0 : 1, nullptr, 0),
						"Game reset acknowledgement failed");
			}
			if (replay.session->Scheduler().Snapshot().permits)
			{
				FramePrediction prediction;
				Require(replay.session->WaitFrame(prediction) == XR_SUCCESS &&
							replay.session->BeginFrame() == XR_SUCCESS,
						"Replay frame admission failed");
				++advancedFrames;
				const auto applied = replay.session->Commands().At(prediction.id);
				Require(reset && applied, "Replay advanced before the game's acknowledged reset");
				if (const auto& body = applied->configuration.tracking.bodyObservation; body && !bodyVerified)
				{
					Require(body->requestedTime.value.count() == applied->runtimeTime.value.count() - 20,
							"Generated replay failed to preserve body observation age in the new runtime clock");
					bodyVerified = true;
				}

				if (applied->configuration.input.controllers[0].primary)
				{
					++presses;
					hit |= applied->configuration.tracking.head.pose.position.x > .5f;
				}
				Require(api.publish(api.context, game, hit ? "hit" : "miss", hit ? 3 : 4),
						"Game outcome publication failed");
				Require(replay.session->EndFrame(prediction) == XR_SUCCESS, "Replay frame completion failed");
			}
			return result;
		});
		auto script = std::string{R"(
local operation, game = simxr.game_command(1, "checkpoint:target")
simxr.await_game(game, operation)
)"} + automation::EditableRig(recording);
		script += "\nsimxr.checkpoint('game outcome', simxr.game_state().telemetry == ";
		script += edit ? "'hit')\n" : "'miss')\n";
		auto options = replay.Options(std::move(script));
		options.seed = recording.seed;
		runner.Start(std::move(options));
		runner.Join();
		if (rejectReset)
		{
			Require(runner.Snapshot().phase == automation::RigPhase::Failed && !reset && advancedFrames == 0 &&
						presses == 0 && runner.Snapshot().checkpoints.empty(),
					"Rejected game reset still replayed or reported success");
			api.unregisterGame(api.context, game);
			continue;
		}
		Require(runner.Snapshot().phase == automation::RigPhase::Complete, runner.Snapshot().error.c_str());
		Require(reset && bodyVerified && presses == 1 && hit == edit && runner.Snapshot().checkpoints.back().passed,
				"Edited playtest did not change the SDK outcome or duplicated the button press");
		api.unregisterGame(api.context, game);
	}
}

void CheckLeaseRenewal()
{
	for (const auto binding : {"state", "game_state", "wait"})
	{
		Fixture fixture;
		DebugControllerConfig config;
		config.leaseDuration = std::chrono::milliseconds{1500};
		const auto controller = fixture.runtime.Debugger(fixture.session, config);
		Require(controller != nullptr, "Lease fixture debugger unavailable");
		unsigned renewals = 0;
		automation::RigRunner runner([&](auto opcode, auto fields) {
			if (opcode == wire::OpCode::Renew)
				++renewals;
			if (opcode == wire::OpCode::SimulationStatus || opcode == wire::OpCode::GameStatus)
				std::this_thread::sleep_for(std::chrono::milliseconds{40});
			return fixture.Exchange(opcode, std::move(fields));
		});
		auto options =
			fixture.Options(std::string_view(binding) == "wait" ? "simxr.pause() simxr.wait(2200) simxr.resume()"
																: "simxr.pause() for i = 1, 55 do simxr." +
																	  std::string(binding) + "() end simxr.resume()");
		options.deadline = std::chrono::seconds{6};
		runner.Start(std::move(options));
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Complete && renewals >= 2,
				"Active rig lost its lease during status polling");
	}
}

void CheckExportReader()
{
	for (unsigned mode = 0; mode < 6; ++mode)
	{
		unsigned releases = 0, polls = 0, waits = 0, reads = 0;
		std::size_t consumed = 0;
		bool failed = false;
		try
		{
			wire::ExportReader reader(
				[&](auto opcode, auto fields) {
					wire::Message reply, request;
					request.fields = std::move(fields);
					if (opcode == wire::OpCode::StartExport)
						reply.fields = {wire::Field::U64(wire::tag::ExportId, 4)};
					else if (opcode == wire::OpCode::ReleaseExport)
						++releases;
					else if (opcode == wire::OpCode::ExportInfo)
					{
						++polls;
						reply.fields = {wire::Field::Text(wire::tag::Phase, polls == 1 ? "pending" : "complete"),
										wire::Field::U64(wire::tag::TotalBytes, 6)};
					}
					else if (opcode == wire::OpCode::ReadExport)
					{
						++reads;
						const auto offset = *request.Find(wire::tag::Offset)->AsU64();
						const auto length = *request.Find(wire::tag::Length)->AsU64();
						Require(length == 6 - offset, "Export read must fit the remaining range");
						std::vector<std::byte> bytes(mode == 1 ? 0 : mode == 2 ? length + 1 : 2, std::byte{7});
						reply.fields = {wire::Field::U64(wire::tag::Offset, mode == 3 ? offset + 1 : offset),
										wire::Field::Bytes(wire::tag::Bytes, bytes)};
					}
					return reply;
				},
				{},
				[&] {
					++waits;
					if (mode == 5)
						throw std::runtime_error("cancelled");
				});
			reader.Read([&](auto bytes) {
				if (mode == 4)
					throw std::runtime_error("sink failed");
				consumed += bytes.size();
			});
			reader.Release();
		}
		catch (const std::runtime_error&)
		{
			failed = true;
		}
		Require(releases == 1 && waits == 1 && failed == (mode != 0),
				"Export ticket cleanup failed during polling, reading, or consumption");
		if (!mode)
			Require(consumed == 6 && reads == 3 && polls == 2,
					"Partial chunks must complete without repolling metadata");
	}
}

void CheckTemporaryResourceCleanup()
{
	for (unsigned mode = 0; mode < 3; ++mode)
	{
		const bool inspect = mode == 1;
		std::atomic<bool> pending = false;
		std::size_t releasedImages = 0, releasedExports = 0, releasedInspections = 0;
		automation::RigRunner runner([&](auto opcode, const auto&) {
			wire::Message response;
			response.fields.push_back(wire::Field::U32(wire::tag::Error, 0));
			switch (opcode)
			{
			case wire::OpCode::RetainImage:
				response.fields.push_back(wire::Field::U64(wire::tag::ImageReaderId, 41));
				break;
			case wire::OpCode::StartExport:
				response.fields.push_back(wire::Field::U64(wire::tag::ExportId, 42));
				response.fields.push_back(wire::Field::U64(wire::tag::TotalBytes, 4));
				break;
			case wire::OpCode::ExportInfo:
				response.fields.push_back(wire::Field::U64(wire::tag::TotalBytes, 4));
				pending = true;
				response.fields.push_back(wire::Field::Text(wire::tag::Phase, mode == 2 ? "pending" : "complete"));
				break;
			case wire::OpCode::ReleaseImage:
				++releasedImages;
				break;
			case wire::OpCode::ReleaseExport:
				++releasedExports;
				break;
			case wire::OpCode::InspectFrame:
				response.fields.push_back(wire::Field::U64(wire::tag::InspectionId, 43));
				response.fields.push_back(wire::Field::Bytes(wire::tag::Document, {}));
				break;
			case wire::OpCode::ReleaseInspection:
				++releasedInspections;
				break;
			case wire::OpCode::Status:
			case wire::OpCode::ReleaseControl:
				break;
			default:
				throw std::runtime_error("Unexpected resource test operation");
			}
			return response;
		});
		automation::RigOptions options;
		options.session = wire::ControlSessionId{1};
		options.generation = 1;
		options.source = inspect ? "simxr.inspect('7')" : "simxr.export('7', '')";
		runner.Start(std::move(options));
		if (mode == 2)
		{
			for (unsigned attempt = 0; attempt < 1000 && !pending; ++attempt)
				std::this_thread::sleep_for(std::chrono::milliseconds{1});
			Require(pending, "Export did not reach pending state before cancellation");
			runner.Cancel();
		}
		runner.Join();
		Require(runner.Snapshot().phase == (mode == 2 ? automation::RigPhase::Cancelled : automation::RigPhase::Failed),
				"Resource operation did not fail or cancel as expected");
		if (inspect)
			Require(releasedInspections == 1, "Malformed inspection leaked or double-released its reader");
		else
			Require(releasedImages == 1 && releasedExports == 1,
					"Failed output open leaked or double-released export resources");
	}
}

void CheckImageWindowCleanup()
{
	for (int mode = 0; mode < 4; ++mode)
	{
		std::size_t releasedWindows = 0, releasedImages = 0, releasedExports = 0;
		std::atomic<bool> begun = false;
		constexpr std::uint64_t window = 9007199254740993ULL;
		automation::RigRunner runner([&](auto opcode, auto fields) {
			wire::Message response;
			response.fields.push_back(wire::Field::U32(wire::tag::Error, 0));
			if (opcode == wire::OpCode::BeginImageWindow)
			{
				begun = true;
				response.fields.push_back(wire::Field::U64(wire::tag::ImageWindowId, window));
				const auto document =
					wire::InspectionValue::Object({{"phase", wire::InspectionValue::Text("complete")}}).Encode();
				response.fields.push_back(
					wire::Field::Bytes(wire::tag::Document, mode == 0 ? std::span<const std::byte>{}
																	  : std::span<const std::byte>{document}));
			}
			else if (opcode == wire::OpCode::RetainWindowImage || opcode == wire::OpCode::ReleaseImageWindow)
			{
				wire::Message request;
				request.fields = std::move(fields);
				Require(!request.Find(wire::tag::Session) && request.Find(wire::tag::ImageWindowId)->AsU64() == window,
						"Window operation lost its exact connection-owned identity");
				if (opcode == wire::OpCode::ReleaseImageWindow)
					++releasedWindows;
				else
					response.fields.push_back(wire::Field::U64(wire::tag::ImageReaderId, 41));
			}
			else if (opcode == wire::OpCode::StartExport)
			{
				response.fields.push_back(wire::Field::U64(wire::tag::ExportId, 42));
				response.fields.push_back(wire::Field::U64(wire::tag::TotalBytes, 4));
			}
			else if (opcode == wire::OpCode::ExportInfo)
			{
				response.fields.push_back(wire::Field::U64(wire::tag::TotalBytes, 4));
				response.fields.push_back(wire::Field::Text(wire::tag::Phase, "complete"));
			}
			else if (opcode == wire::OpCode::ReleaseImage)
				++releasedImages;
			else if (opcode == wire::OpCode::ReleaseExport)
				++releasedExports;
			else
				Require(opcode == wire::OpCode::ReleaseControl || opcode == wire::OpCode::Status,
						"Unexpected window cleanup operation");
			return response;
		});
		automation::RigOptions options;
		options.session = wire::ControlSessionId{1};
		options.generation = 1;
		options.source = "local document, window = simxr.begin_image_window({target='quad_layers'})\n"
						 "assert(window == '9007199254740993' and document.phase == 'complete')\n";
		options.source += mode == 2	  ? "simxr.release_image_window(window)"
						  : mode == 3 ? "while true do end"
									  : "simxr.export_window_image(window, '7', '')";
		runner.Start(std::move(options));
		if (mode == 3)
		{
			for (int attempt = 0; attempt < 1000 && !begun; ++attempt)
				std::this_thread::sleep_for(std::chrono::milliseconds{1});
			Require(begun, "Rig did not acquire its window before cancellation");
			runner.Cancel();
		}
		runner.Join();
		Require(releasedWindows == 1 && releasedImages == (mode == 1 ? 1 : 0) && releasedExports == (mode == 1 ? 1 : 0),
				"Window/export ownership leaked or was released twice");
		Require(runner.Snapshot().phase == (mode == 2	? automation::RigPhase::Complete
											: mode == 3 ? automation::RigPhase::Cancelled
														: automation::RigPhase::Failed),
				"Window cleanup test did not exercise its expected result");
	}
}

void CheckExactFrameArithmetic()
{
	Fixture fixture;
	automation::RigRunner runner([&](auto opcode, auto fields) { return fixture.Exchange(opcode, std::move(fields)); });
	runner.Start(fixture.Options(R"(
assert(simxr.frame_add("9007199254740993", "2") == "9007199254740995")
assert(simxr.frame_add("18446744073709551614", "1") == "18446744073709551615")
assert(simxr.frame_before("9007199254740992", "9007199254740993"))
assert(not simxr.frame_before("18446744073709551615", "18446744073709551615"))
assert(not pcall(simxr.frame_add, "18446744073709551615", "1"))
assert(not pcall(simxr.frame_add, "01", "1"))
assert(not pcall(simxr.frame_before, 9007199254740992, "1"))
simxr.checkpoint("exact frame arithmetic", true)
)"));
	runner.Join();
	Require(runner.Snapshot().phase == automation::RigPhase::Complete, runner.Snapshot().error.c_str());
}

void CheckEditableCommands()
{
	CommandConfiguration configuration;
	configuration.tracking.head.flags = std::numeric_limits<std::uint64_t>::max();
	auto& controls = configuration.input.controllers[1];
	controls.trigger = .13579f;
	controls.grip = .25f;
	controls.gripForce = .75f;
	controls.stick = {-.125f, .5f};
	controls.trackpad = {.875f, -.25f};
	controls.trackpadForce = .625f;
	controls.primary = controls.secondary = controls.menu = controls.back = true;
	controls.thumbClick = controls.trackpadClick = true;
	controls.primaryTouch = controls.secondaryTouch = controls.triggerTouch = true;
	controls.thumbTouch = controls.trackpadTouch = controls.thumbrestTouch = true;
	controls.connected = false;
	controls.triggerClick = false;
	controls.physicalProfile = "/interaction_profiles/valve/index_controller";
	auto& body = configuration.tracking.body;
	body.active = false;
	body.confidence = .625f;
	body.preset = BodyPreset::Seated;
	body.rootPose = XrPosef{EulerAngles(.4f, 0, 0), {2, .1f, -3}};
	body.controls[0].offset = XrVector3f{.125f, -.25f, .75f};
	body.controls[4].tracked = false;
	body.joints[3] = BodyJointOverride{XrPosef{{.1f, .2f, .3f, .9f}, {.5f, .25f, -.125f}}, false};
	body.joints[7] = BodyJointOverride{std::nullopt, false};
	auto& tracker = configuration.tracking.trackers[2];
	tracker.identity.value = std::numeric_limits<std::uint64_t>::max();
	tracker.poseFlags = XR_SPACE_LOCATION_ORIENTATION_VALID_BIT;
	tracker.poseOverride = XrPosef{{0, 0, 0, 1}, {3, 2, 1}};
	tracker.anchor.reset();
	tracker.connected = tracker.tracked = false;

	TrackerDevice attached = configuration.tracking.trackers[0];
	attached.anchor = BodyJoint::Chest;
	attached.jointOffset = XrPosef{EulerAngles(.2f, .3f, -.1f), {.1f, -.2f, .3f}};
	configuration.views.ipd = .071f;
	configuration.tracking.controllers[1].aim = PoseSample{};
	configuration.tracking.controllers[1].aim->pose.position = {3, 2, 1};
	configuration.tracking.controllers[1].aim->flags = ValidPoseFlags;
	configuration.views.outerWidth = 321;
	configuration.views.insetHalfAngle = .275f;
	configuration.views.outerFov[1] = {-.6f, .7f, .8f, -.9f};
	StereoEyeViews observed;
	observed.spaceRevision = std::numeric_limits<std::uint64_t>::max();
	observed.poses[0].pose = {EulerAngles(.1f, .2f, .3f), {-.04f, 1.65f, .01f}};
	observed.poses[0].flags = TrackedPoseFlags;
	observed.fovs[1] = {-.7f, .6f, .8f, -.9f};
	std::vector<SimulationCommand> expected{SetEyeViews{observed},
											SetEyeViews{},
											TakeControl{ControlChannel::Head, true},
											ReleaseControl{ControlChannel::Gaze},
											SetHeadPose{configuration.tracking.head},
											SetControllerPose{1, configuration.tracking.controllers[1]},
											SetControllerControls{1, controls},
											SetBodyControls{body},
											SetGaze{{-.125f, .625f, false, true}},
											SetTracker{2, tracker},
											SetTracker{0, attached},
											SetViewSettings{configuration.views},
											SetBodyProportions{{1.9f, .4f, .3f}},
											SetControllerProfile{1, "/quote\"/slash\\/line\n/Grüße"},
											SetScenario{FailureScenario::Hidden, false}};
	SetTrackers collection;
	for (std::size_t index = 0; index < MaxTrackerDevices; ++index)
		collection.value.push_back({{100 + index}, TrackerRole::Unassigned});
	collection.value.back().sourcePath = TrackerSourcePath{"htcx:/device/stable"};
	expected.push_back(collection);
	expected.push_back(SetTrackers{});
	for (const auto set :
		 {BodyJointSet::UpperBodyFB, BodyJointSet::FullBodyMETA, BodyJointSet::WithoutArmsBD, BodyJointSet::FullBodyBD})
	{
		BodyObservation observation;
		observation.jointSet = set;
		observation.requestedTime = {Duration{9007199254740993LL}};
		observation.located = true;
		const bool fb = set == BodyJointSet::UpperBodyFB || set == BodyJointSet::FullBodyMETA;
		if (fb)
		{
			observation.active = false;
			observation.confidence = 0;
			observation.returnedTime = RuntimeTime{Duration{9007199254740991LL}};
			observation.skeletonRevision = 0;
			observation.skeleton.emplace();
		}
		else
			observation.allJointPosesTracked = false;
		for (const auto joint : BodyModel::JointMap(set))
		{
			const auto index = static_cast<std::size_t>(joint);
			observation.joints[index] =
				ObservedBodyJoint{IdentityPose(), fb ? 0 : XR_SPACE_LOCATION_ORIENTATION_VALID_BIT};
			observation.joints[index]->pose.position.x = static_cast<float>(index);
			if (fb)
				(*observation.skeleton)[index] = SkeletonJoint{joint, {}, IdentityPose()};
		}
		expected.push_back(SetBodyControls{{}, observation});
		expected.push_back(SetBodyControls{{}, observation, RuntimeTime{Duration{9007199254740993LL}}});
	}
	std::string script = "simxr.schedule({frame=\"1\", commands={\n";
	for (const auto& command : expected)
		script += automation::EditableCommand(command) + ",\n";
	script += "}})";
	std::vector<SimulationCommand> decoded;
	automation::RigRunner runner([&](auto opcode, auto fields) {
		wire::Message response;
		response.fields.push_back(wire::Field::U32(wire::tag::Error, 0));
		if (opcode == wire::OpCode::SubmitSimulation)
		{
			wire::Message request;
			request.fields = std::move(fields);
			const auto commands = wire::DecodeFields(request.Find(wire::tag::Commands)->value);
			for (const auto& command : commands.fields)
				decoded.push_back(wire::DecodeSimulationCommand(command));
			response.fields.push_back(wire::Field::Text(wire::tag::Phase, "queued"));
		}
		return response;
	});
	automation::RigOptions options;
	options.session = wire::ControlSessionId{1};
	options.generation = 1;
	options.source = std::move(script);
	runner.Start(std::move(options));
	runner.Join();
	Require(runner.Snapshot().phase == automation::RigPhase::Complete, runner.Snapshot().error.c_str());
	Require(decoded.size() == expected.size(), "Editable commands lost a command variant");
	for (std::size_t index = 0; index < expected.size(); ++index)
		Require(wire::EncodeSimulationCommand(expected[index]).value ==
					wire::EncodeSimulationCommand(decoded[index]).value,
				"Editable command table changed semantic command data");
}

void CheckBodyReplayTransport()
{
	Fixture fixture;
	BodyObservation body;
	body.jointSet = BodyJointSet::WithoutArmsBD;
	body.requestedTime = {Duration{9007199254740973LL}};
	body.located = true;
	body.allJointPosesTracked = false;
	for (const auto joint : BodyModel::JointMap(body.jointSet))
		body.joints[static_cast<std::size_t>(joint)] = ObservedBodyJoint{IdentityPose(), ValidPoseFlags};
	std::shared_ptr<const AppliedCommandSnapshot> applied;
	automation::RigRunner runner([&](auto opcode, auto fields) {
		auto response = fixture.Exchange(opcode, std::move(fields));
		if (opcode == wire::OpCode::SubmitSimulation)
			applied = fixture.session->Commands().Snapshot().applied;
		return response;
	});
	const auto command =
		automation::EditableCommand(SetBodyControls{{}, body, RuntimeTime{Duration{9007199254740993LL}}});
	runner.Start(fixture.Options("simxr.schedule({frame='1', commands={{kind='take_control',channel='body'}, " +
								 command + "}})"));
	runner.Join();
	Require(runner.Snapshot().phase == automation::RigPhase::Complete && applied &&
				applied->configuration.tracking.bodyObservation &&
				applied->configuration.tracking.bodyObservation->requestedTime.value.count() ==
					applied->runtimeTime.value.count() - 20,
			"Luau/transport/application boundary lost the recorded body sample age");
}

void CheckMalformedBodyObservations()
{
	BodyObservation body;
	body.jointSet = BodyJointSet::UpperBodyFB;
	body.requestedTime = {Duration{1}};
	body.located = true;
	body.active = false;
	body.confidence = 0;
	body.returnedTime = RuntimeTime{};
	body.skeletonRevision = 0;
	body.skeleton.emplace();
	for (const auto joint : BodyModel::JointMap(body.jointSet))
	{
		const auto index = static_cast<std::size_t>(joint);
		body.joints[index] = ObservedBodyJoint{IdentityPose(), 0};
		(*body.skeleton)[index] = SkeletonJoint{joint, {}, IdentityPose()};
	}
	for (const auto mutation :
		 {"b.joint_set = 256", "b.requested_time = '9223372036854775808'", "b.requested_time = 9007199254740994",
		  "b.joints[1].flags = nil", "b.joints[1] = nil", "b.joints.extra = {}",
		  "b.joints[2].joint = b.joints[1].joint", "b.skeleton[1].parent = 256",
		  "b.skeleton[1].parent = b.skeleton[1].joint", "b.skeleton[2].joint = b.skeleton[1].joint", "b.active = nil"})
	{
		bool submitted = false;
		automation::RigRunner runner([&](auto opcode, const auto&) {
			submitted |= opcode == wire::OpCode::SubmitSimulation;
			wire::Message response;
			response.fields.push_back(wire::Field::U32(wire::tag::Error, 0));
			return response;
		});
		automation::RigOptions options;
		options.session = wire::ControlSessionId{1};
		options.generation = 1;
		options.source = "local command = " + automation::EditableCommand(SetBodyControls{{}, body}) +
						 "; local b = command.observation; " + mutation +
						 "; simxr.schedule({frame='1', commands={command}})";
		runner.Start(std::move(options));
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Failed && !submitted,
				"Malformed observed body reached submission");
	}
}

void CheckUnreportedCompatibility()
{
	Fixture fixture;
	automation::RigRunner runner([&](auto opcode, auto fields) {
		auto response = fixture.Exchange(opcode, std::move(fields));
		std::erase_if(response.fields,
					  [](const auto& field) { return field.tag == wire::tag::CompatibilityDeviations; });
		return response;
	});
	runner.Start(fixture.Options("simxr.checkpoint('older runtime', true)"));
	runner.Join();
	Require(runner.Snapshot().phase == automation::RigPhase::Complete &&
				runner.JsonReport().find("\"compatibility_deviations\":null") != std::string::npos &&
				runner.JunitReport().find("value=\"unknown\"") != std::string::npos,
			"Older runtime was falsely reported as having no deviations");
}

int main()
{
	try
	{
		CheckUnreportedCompatibility();
		CheckSweepRigs();
		CheckSharedSequenceRigCancellation();
		CheckTransportRetirementAfterCleanupFailure();
		CheckControllerBindingRigOwnership();
		Fixture fixture;
		automation::RigRunner runner(
			[&](auto opcode, auto fields) { return fixture.Exchange(opcode, std::move(fields)); });
		runner.Start(fixture.Options(R"(
local command = simxr.schedule({frame="1", commands={
    {kind="take_control",channel="head"},
    {kind="head_pose",position={1,1.7,-2}},
    {kind="take_control",channel="left_controls"},
    {kind="controller_controls",hand=0,trigger=0.75,primary=true},
}})
local applied = simxr.await_transaction(command)
simxr.checkpoint("exact input frame", applied.applied_frame == "1")
local state = simxr.state()
simxr.checkpoint("head pose", state.configuration.head.position[1] == 1)
print("seed", simxr.seed)
)"));
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Complete, runner.Snapshot().error.c_str());
		Require(runner.Snapshot().transactions == 1 && runner.Snapshot().checkpoints.size() == 2,
				"Actual Luau bindings/checkpoints did not run");
		Require(runner.JsonReport().find("\"seed\":\"73\"") != std::string::npos &&
					runner.JunitReport().find("failures=\"0\"") != std::string::npos &&
					runner.JsonReport().find("\"compatibility_deviations\":[]") != std::string::npos &&
					runner.JunitReport().find("name=\"compatibility_deviations\" value=\"none\"") != std::string::npos,
				"Run reports");
		const auto applied = fixture.session->Commands().At(FrameId{1});
		Require(applied && applied->configuration.tracking.head.pose.position.x == 1 &&
					applied->configuration.input.controllers[0].trigger == .75f,
				"Commands did not apply in shared runtime");
		FramePrediction next;
		Require(fixture.session->WaitFrame(next) == XR_SUCCESS &&
					fixture.session->Input().Snapshot().controllers[0].trigger == 0,
				"Runner completion retained held input");
		Require(fixture.session->BeginFrame() == XR_SUCCESS && fixture.session->EndFrame(next) == XR_SUCCESS,
				"Finish cleanup frame");
		runner.Start(
			fixture.Options("local x: number = 'syntax type checking is not required'\nerror('useful failure')"));
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Failed &&
					runner.Snapshot().error.find("test-rig.luau:2") != std::string::npos,
				"Script location lost");
		Require(runner.JunitReport().find("failure message=") != std::string::npos, "Failure report");
		runner.Start(fixture.Options("simxr.checkpoint('quote<&', false, 'bad<&')"));
		runner.Join();
		Require(runner.JunitReport().find("quote&lt;&amp;") != std::string::npos, "JUnit escaping");
		runner.Start(fixture.Options("while true do end"));
		runner.Cancel();
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Cancelled, "Pure Luau loop ignored cancellation");
		auto bounded = fixture.Options("while true do end");
		bounded.deadline = std::chrono::milliseconds{20};
		runner.Start(std::move(bounded));
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Failed &&
					runner.Snapshot().error.find("deadline") != std::string::npos,
				"Pure Luau loop ignored deadline");
		auto memoryLimited = fixture.Options("local values = table.create(500000, false)");
		memoryLimited.memoryLimit = 1024 * 1024;
		runner.Start(std::move(memoryLimited));
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Failed, "VM allocation exceeded memory budget");
		runner.Start(fixture.Options(R"(
local transaction = simxr.schedule({frame="3", commands={
    {kind="take_control", channel="head"},
    {kind="head_pose", position={3, 1.7, 0}},
}})
local applied = simxr.await_transaction(transaction)
simxr.checkpoint("fresh VM, persistent IDs", applied.applied_frame == "3")
)"));
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Complete, runner.Snapshot().error.c_str());
		Require(fixture.session->Commands().At(FrameId{3})->configuration.tracking.head.pose.position.x == 3,
				"Restart reused transaction IDs or retained failed VM state");
		CheckBindingPauseAndStep();
		CheckResumeClearsSteps();
		CheckTimedWaits();
		CheckPausedCancellation();
		CheckSteppedExecutionPhase();
		CheckCleanupFailures();
		CheckScriptValueBoundaries();
		CheckReportText();
		CheckExportReader();
		CheckTemporaryResourceCleanup();
		CheckImageWindowCleanup();
		CheckLeaseRenewal();
		CheckInputRecording();
		CheckRecordedPlaytestOutcome();
		CheckExactFrameArithmetic();
		CheckEditableCommands();
		CheckBodyReplayTransport();
		CheckMalformedBodyObservations();
		std::cout << "PASS Luau worker, exact shared input, cleanup, reports and cancellation\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL " << error.what() << '\n';
		return 1;
	}
}
