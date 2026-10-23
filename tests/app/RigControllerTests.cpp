#include "../TestSupport.h"
#include "app/services/RigController.h"
#include "app/services/Utf8Path.h"
#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "runtime/Runtime.h"

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>

namespace app = simxr::app;
namespace automation = simxr::automation;

static void Write(const std::filesystem::path& path, const std::string& source)
{
	std::ofstream file(path, std::ios::binary);
	file << source;
	file.close();
	Require(bool(file), "Write rig fixture");
}
static std::string Read(const std::filesystem::path& path)
{
	std::ifstream file(path, std::ios::binary);
	Require(bool(file), "Read saved report");
	return {std::istreambuf_iterator<char>(file), {}};
}
static automation::RigSnapshot Finished(app::RigController& rigs)
{
	Eventually(
		[&]
		{
			return !rigs.IsActive();
		},
		"Rig controller did not settle");
	return rigs.Snapshot();
}
static app::RigReportResult Report(app::RigController& rigs)
{
	std::optional<app::RigReportResult> result;
	Eventually(
		[&]
		{
			result = rigs.TakeReportResult();
			return result.has_value();
		},
		"Report worker did not settle");
	return std::move(*result);
}
static void Check(const std::filesystem::path& root, const std::string& endpoint)
{
	simxr::Runtime runtime{simxr::ExtensionRegistry{std::span<const simxr::ExtensionDefinition>{}}};
	simxr::Runtime::InstanceRef instance;
	simxr::Runtime::SessionRef session;
	XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
	std::strcpy(create.applicationInfo.applicationName, "Rig controller test");
	create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
	Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
				runtime.CreateSession(instance, session) == XR_SUCCESS,
			"Create rig session");
	simxr::FrameSchedulerConfig config;
	config.mode = simxr::ExecutionMode::Deterministic;
	XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
	Require(session->Ready(config.epoch) == XR_SUCCESS && session->Begin(begin, config) == XR_SUCCESS,
			"Begin rig session");
	simxr::control::ControlService service(runtime);
	simxr::control::LocalControlServer server(service, endpoint);
	const app::RigTarget target{server.Path(), 1, session->Snapshot().generation.value};
	const auto script = root / app::Utf8Path("目-script.luau");
	Write(script, "while true do end");
	app::RigController rigs;
	rigs.RunScript(target, app::PathText(script), {73, 30, 64});
	Require(rigs.Error().empty() && rigs.IsActive() && rigs.RunId() == 1 && !rigs.ReplayingTake(),
			"Start real script worker");
	rigs.ReplayRecording(target, app::PathText(script), 999);
	Require(!rigs.Error().empty() && rigs.RunId() == 1 && !rigs.ReplayingTake() && rigs.Snapshot().seed == 73 &&
				rigs.Script() == app::PathText(script) && rigs.MatchesTarget(target),
			"Rejected replay relabelled the active script");
	Require(!rigs.SaveReport(app::PathText(root / "active.json"), false).empty(),
			"Saved a report before execution settled");
	rigs.Cancel();
	Require(Finished(rigs).phase == automation::RigPhase::Cancelled, "Cancel did not settle the active run");
	Write(script, "simxr.checkpoint('retained launch bytes', true)");
	rigs.ReplayRecording(target, app::PathText(script), 999);
	Require(rigs.Error().empty() && rigs.ReplayingTake() && Finished(rigs).phase == automation::RigPhase::Complete,
			"Replay did not complete after a cancelled script");
	const auto saved = root / app::Utf8Path("目-report.json");
	Require(rigs.SaveReport(app::PathText(saved), false).empty(), "Admit report write");
	Require(!rigs.SaveReport(app::PathText(root / "overwritten.json"), false).empty(),
			"Unread report result was replaced");
	app::LaunchProfile profile;
	profile.rig = app::PathText(script.filename());
	profile.workingDirectory = app::PathText(root);
	rigs.PrepareLaunch(profile);
	Require(rigs.WaitingForLaunch(), "Launch source was not retained");
	std::filesystem::remove(script);
	rigs.AttachLaunch({target.endpoint, target.session, target.generation});
	Require(!rigs.WaitingForLaunch() && !rigs.ReplayingTake() && rigs.RunId() == 3 && rigs.Snapshot().seed == 0 &&
				Finished(rigs).phase == automation::RigPhase::Complete,
			"Launch inherited replay settings or lost deferred source");
	const auto report = Report(rigs);
	Require(!report.error && report.path == app::PathText(saved) && Read(saved).find("999") != std::string::npos,
			"Deferred report lost the previous run or Unicode destination");
	const auto previous = rigs.Snapshot();
	rigs.RunScript(target, app::PathText(script), {123});
	Require(!rigs.Error().empty() && rigs.RunId() == 3 && rigs.Snapshot().seed == previous.seed &&
				!rigs.ReplayingTake(),
			"Missing script mutated the last accepted run");
	const auto blocked = root / "blocked.json";
	std::filesystem::create_directory(blocked);
	Write(blocked / "keep", "existing content");
	Require(rigs.SaveReport(app::PathText(blocked), false).empty(), "Admit failing report commit");
	Require(Report(rigs).error != nullptr && Read(blocked / "keep") == "existing content",
			"Failed report damaged its destination");
	for (const auto& file : std::filesystem::directory_iterator(root))
		Require(!app::PathText(file.path().filename()).starts_with("blocked.json."),
				"Failed report left temporary output");
	Require(rigs.SaveReport(app::PathText(root / "report.xml"), true).empty(), "Admit JUnit report");
	Require(!Report(rigs).error && Read(root / "report.xml").find("retained launch bytes") != std::string::npos,
			"JUnit report lost its checkpoint");
	Write(script, "simxr.checkpoint('saved at shutdown', true)");
	{
		app::RigController closing;
		closing.RunScript(target, app::PathText(script), {});
		Require(Finished(closing).phase == automation::RigPhase::Complete, "Prepare shutdown report");
		Require(closing.SaveReport(app::PathText(root / "shutdown.json"), false).empty(), "Admit shutdown report");
	}
	Require(Read(root / "shutdown.json").find("saved at shutdown") != std::string::npos,
			"Destruction abandoned accepted report bytes");
}
int main()
{
	const auto name =
		"simxr-rig-controller-" + std::to_string(std::chrono::steady_clock::now().time_since_epoch().count());
	const auto root = std::filesystem::temp_directory_path() / name;
	struct Cleanup
	{
		std::filesystem::path path;
		~Cleanup()
		{
			std::error_code error;
			std::filesystem::remove_all(path, error);
		}
	} cleanup{root};
	try
	{
		std::filesystem::create_directory(root);
#ifdef _WIN32
		Check(root, R"(\\.\pipe\SimXR.Control.)" + name);
#else
		Check(root, (root / "control.sock").string());
#endif
		std::cout << "Rig controller tests passed\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
