#include "../TestSupport.h"
#include "Math.h"
#include "automation/Recording.h"
#include "automation/RigRunner.h"
#include "automation/RigProgress.h"
#include "automation/RigReport.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "control/InspectionDocument.h"
#include "control/SimulationControl.h"
#include "runtime/Runtime.h"

#include <algorithm>
#include <atomic>
#include <cstring>
#include <iostream>
#include <limits>
#include <locale>
#include <thread>

using namespace simxr;
namespace wire = control;

struct Fixture
{
	Runtime runtime{ExtensionRegistry{std::span<const ExtensionDefinition>{}}};
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	wire::ControlService service{runtime};
	wire::ConnectionId connection;
	std::uint64_t request = 0;
	Fixture()
	{
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		std::strcpy(create.applicationInfo.applicationName, "Luau runner integration");
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
		Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
					runtime.CreateSession(instance, session) == XR_SUCCESS,
				"Fixture create");
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
void CheckBindingPauseAndStep()
{
	Fixture fixture;
	std::atomic<bool> stateEntered = false;
	std::atomic<bool> continueState = false;
	std::atomic<bool> intercepted = false;
	automation::RigRunner runner(
		[&](auto opcode, auto fields)
		{
			if (opcode == wire::OpCode::SimulationStatus && !intercepted.exchange(true))
			{
				stateEntered = true;
				Eventually(
					[&]
					{
						return continueState.load();
					},
					"State interception was not released");
			}
			return fixture.Exchange(opcode, std::move(fields));
		});
	runner.Start(fixture.Options("simxr.state()\nprint('one binding')\nsimxr.checkpoint('after resume', true)"));
	try
	{
		Eventually(
			[&]
			{
				return stateEntered.load();
			},
			"Script did not enter state binding");
		runner.Pause();
		continueState = true;
		Eventually(
			[&]
			{
				return runner.Snapshot().phase == automation::RigPhase::Paused;
			},
			"Runner did not pause at binding boundary");
		runner.Step();
		Eventually(
			[&]
			{
				return runner.Snapshot().output.size() == 1;
			},
			"One binding permit did not run print");
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

void CheckCleanupFailures()
{
	for (const auto failed : {wire::OpCode::ReleaseImageWindow, wire::OpCode::ReleaseInspection,
		wire::OpCode::Relinquish})
	{
		Fixture fixture;
		bool attempted = false, inputsReleased = false;
		automation::RigRunner runner([&](auto opcode, auto fields)
		{
			if (opcode == wire::OpCode::ReleaseControl)
				inputsReleased = true;
			if (opcode == failed)
			{
				attempted = true;
				wire::Message response;
				response.fields = {wire::Field::U32(wire::tag::Error, static_cast<std::uint32_t>(wire::ErrorCode::Internal)),
					wire::Field::Text(wire::tag::Detail, "injected cleanup failure")};
				return response;
			}
			if (opcode == wire::OpCode::BeginImageWindow || opcode == wire::OpCode::InspectFrame)
			{
				wire::Message response;
				response.fields = {wire::Field::U32(wire::tag::Error, 0),
					wire::Field::U64(opcode == wire::OpCode::BeginImageWindow ? wire::tag::ImageWindowId : wire::tag::InspectionId, 42),
					wire::Field::Bytes(wire::tag::Document, wire::InspectionValue::Object({}).Encode())};
				return response;
			}
			return fixture.Exchange(opcode, std::move(fields));
		});
		runner.Start(fixture.Options(failed == wire::OpCode::ReleaseImageWindow ? "simxr.begin_image_window()" :
			failed == wire::OpCode::ReleaseInspection ? "simxr.inspect('7')" : "simxr.pause()"));
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
	automation::RigRunner runner([&](auto opcode, auto fields)
	{
		if (opcode == wire::OpCode::SubmitSimulation)
			++submissions;
		if (opcode == wire::OpCode::SimulationStatus)
		{
			wire::Message response;
			const auto document = wire::EncodeFields(std::vector{wire::Field::Text(1, ""), wire::Field::U32(2, 999),
				wire::Field::Bytes(3, {})});
			response.fields = {wire::Field::U32(wire::tag::Error, 0), wire::Field::Bytes(wire::tag::Document, document)};
			return response;
		}
		return fixture.Exchange(opcode, std::move(fields));
	});
	runner.Start(fixture.Options(R"(
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
		std::string do_grouping() const override { return "\3"; }
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

void CheckLeaseRenewal()
{
	for (const auto binding : {"state", "game_state"})
	{
		Fixture fixture;
		DebugControllerConfig config;
		config.leaseDuration = std::chrono::milliseconds{1500};
		const auto controller = fixture.runtime.Debugger(fixture.session, config);
		Require(controller != nullptr, "Lease fixture debugger unavailable");
		unsigned renewals = 0;
		automation::RigRunner runner(
			[&](auto opcode, auto fields)
			{
				if (opcode == wire::OpCode::Renew)
					++renewals;
				if (opcode == wire::OpCode::SimulationStatus || opcode == wire::OpCode::GameStatus)
					std::this_thread::sleep_for(std::chrono::milliseconds{40});
				return fixture.Exchange(opcode, std::move(fields));
			});
		auto options = fixture.Options("simxr.pause() for i = 1, 55 do simxr." + std::string(binding) + "() end simxr.resume()");
		options.deadline = std::chrono::seconds{6};
		runner.Start(std::move(options));
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Complete && renewals >= 2,
			"Active rig lost its lease during status polling");
	}
}

void CheckTemporaryResourceCleanup()
{
	for (const bool inspect : {false, true})
	{
		std::size_t releasedImages = 0, releasedExports = 0, releasedInspections = 0;
		automation::RigRunner runner(
			[&](auto opcode, auto)
			{
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
					response.fields.push_back(wire::Field::Text(wire::tag::Phase, "complete"));
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
		runner.Join();
		Require(runner.Snapshot().phase == automation::RigPhase::Failed, "Invalid resource operation succeeded");
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
		automation::RigRunner runner([&](auto opcode, auto fields)
		{
			wire::Message response;
			response.fields.push_back(wire::Field::U32(wire::tag::Error, 0));
			if (opcode == wire::OpCode::BeginImageWindow)
			{
				begun = true;
				response.fields.push_back(wire::Field::U64(wire::tag::ImageWindowId, window));
				const auto document = wire::InspectionValue::Object({{"phase", wire::InspectionValue::Text("complete")}}).Encode();
				response.fields.push_back(wire::Field::Bytes(wire::tag::Document,
					mode == 0 ? std::span<const std::byte>{} : std::span<const std::byte>{document}));
			}
			else if (opcode == wire::OpCode::RetainWindowImage || opcode == wire::OpCode::ReleaseImageWindow)
			{
				wire::Message request;
				request.fields = std::move(fields);
				Require(!request.Find(wire::tag::Session) &&
					request.Find(wire::tag::ImageWindowId)->AsU64() == window,
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
				response.fields.push_back(wire::Field::Text(wire::tag::Phase, "complete"));
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
		options.source += mode == 2 ? "simxr.release_image_window(window)"
			: mode == 3 ? "while true do end" : "simxr.export_window_image(window, '7', '')";
		runner.Start(std::move(options));
		if (mode == 3)
		{
			for (int attempt = 0; attempt < 1000 && !begun; ++attempt)
				std::this_thread::sleep_for(std::chrono::milliseconds{1});
			Require(begun, "Rig did not acquire its window before cancellation");
			runner.Cancel();
		}
		runner.Join();
		Require(releasedWindows == 1 && releasedImages == (mode == 1 ? 1 : 0) &&
			releasedExports == (mode == 1 ? 1 : 0), "Window/export ownership leaked or was released twice");
		Require(runner.Snapshot().phase == (mode == 2 ? automation::RigPhase::Complete :
			mode == 3 ? automation::RigPhase::Cancelled : automation::RigPhase::Failed),
			"Window cleanup test did not exercise its expected result");
	}
}

void CheckExactFrameArithmetic()
{
	Fixture fixture;
	automation::RigRunner runner(
		[&](auto opcode, auto fields)
		{
			return fixture.Exchange(opcode, std::move(fields));
		});
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
	tracker.poseOverride = XrPosef{{0, 0, 0, 1}, {3, 2, 1}};
	tracker.anchor.reset();
	tracker.connected = tracker.tracked = false;

	TrackerDevice attached = configuration.tracking.trackers[0];
	attached.anchor = BodyJoint::Chest;
	attached.jointOffset = XrPosef{EulerAngles(.2f, .3f, -.1f), {.1f, -.2f, .3f}};
	configuration.views.ipd = .071f;
	configuration.views.outerWidth = 321;
	configuration.views.insetHalfAngle = .275f;
	configuration.views.outerFov[1] = {-.6f, .7f, .8f, -.9f};
	std::vector<SimulationCommand> expected{TakeControl{ControlChannel::Head, true},
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
	std::string script = "simxr.schedule({frame=\"1\", commands={\n";
	for (const auto& command : expected)
		script += automation::EditableCommand(command) + ",\n";
	script += "}})";
	std::vector<SimulationCommand> decoded;
	automation::RigRunner runner(
		[&](auto opcode, auto fields)
		{
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

void CheckUnreportedCompatibility()
{
	Fixture fixture;
	automation::RigRunner runner([&](auto opcode, auto fields)
	{
		auto response = fixture.Exchange(opcode, std::move(fields));
		std::erase_if(response.fields, [](const auto& field) { return field.tag == wire::tag::CompatibilityDeviations; });
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
		Fixture fixture;
		automation::RigRunner runner(
			[&](auto opcode, auto fields)
			{
				return fixture.Exchange(opcode, std::move(fields));
			});
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
		CheckCleanupFailures();
		CheckScriptValueBoundaries();
		CheckReportText();
		CheckTemporaryResourceCleanup();
		CheckImageWindowCleanup();
		CheckLeaseRenewal();
		CheckInputRecording();
		CheckExactFrameArithmetic();
		CheckEditableCommands();
		std::cout << "PASS Luau worker, exact shared input, cleanup, reports and cancellation\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL " << error.what() << '\n';
		return 1;
	}
}
