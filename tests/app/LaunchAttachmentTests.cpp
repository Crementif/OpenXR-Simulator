#include "../TestSupport.h"
#include "app/services/LaunchAttachment.h"
#include "app/services/RuntimeEndpoints.h"
#include "app/services/RuntimeConnection.h"
#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "runtime/Runtime.h"

#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <string_view>
#include <unistd.h>

namespace
{
using namespace simxr;
namespace app = simxr::app;
namespace wire = simxr::control;

struct Directory
{
	std::filesystem::path path;

	Directory()
	{
		char pattern[] = "/tmp/simxr-rig-launch-XXXXXX";
		Require(::mkdtemp(pattern) != nullptr, "Create launch directory");
		path = pattern;
	}

	~Directory()
	{
		std::error_code ignored;
		std::filesystem::remove_all(path, ignored);
	}

	app::LaunchProfile Profile() const
	{
		app::LaunchProfile profile;
		profile.environment = {{"SIMXR_DATA_DIR", path.string()}};
		return profile;
	}

	std::string Endpoint() const
	{
		return (path / ("control." + std::to_string(::getpid()) + ".sock")).string();
	}
};

struct RuntimeFixture
{
	Runtime runtime{ExtensionRegistry{std::span<const ExtensionDefinition>{}}};
	Runtime::InstanceRef instance;
	Runtime::SessionRef session;
	wire::ControlService service{runtime};
	std::unique_ptr<wire::LocalControlServer> server;

	RuntimeFixture()
	{
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
		std::strcpy(create.applicationInfo.applicationName, "Launch attachment fixture");
		Require(runtime.CreateInstance(&create, instance) == XR_SUCCESS &&
					runtime.CreateSession(instance, session) == XR_SUCCESS,
				"Create runtime session");
	}

	void Listen(const std::string& endpoint)
	{
		server = std::make_unique<wire::LocalControlServer>(service, endpoint);
	}

	void Begin()
	{
		FrameSchedulerConfig timing;
		timing.mode = ExecutionMode::Deterministic;
		XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		Require(session->Ready(timing.epoch) == XR_SUCCESS && session->Begin(begin, timing) == XR_SUCCESS,
				"Begin launched session");
	}
};

template<class Work>
void Reject(Work&& work, std::string_view reason)
{
	try
	{
		work();
	}
	catch (const std::exception& failure)
	{
		Require(std::string_view(failure.what()).find(reason) != std::string_view::npos,
				"Unexpected attachment rejection");
		return;
	}
	throw std::runtime_error("Invalid launch attachment was accepted");
}

app::LaunchSnapshot Running()
{
	app::LaunchSnapshot process;
	process.state = app::LaunchState::Running;
	process.process = static_cast<std::uint64_t>(::getpid());
	return process;
}

void FreshEndpointAndRunningSession()
{
	Directory directory;
	RuntimeFixture runtime;
	app::RuntimeConnection discovery;
	std::uint64_t completedEpoch;
	{
		app::LaunchAttachment attachment{discovery, directory.Profile()};
		Require(!attachment.Poll(), "Unobserved launch attached to another runtime");
		attachment.Observe(Running());
		Require(!attachment.Poll(), "Absent endpoint attached");
		runtime.Listen(directory.Endpoint());
		Eventually(
			[&] {
				Require(!attachment.Poll(), "Non-running session attached");
				return discovery.Snapshot().state == app::RuntimeConnectionState::Connected;
			},
			"Discovery did not connect asynchronously");
		runtime.Begin();
		std::optional<app::LaunchedSession> result;
		Eventually(
			[&] {
				result = attachment.Poll();
				return result.has_value();
			},
			"New running session did not attach");
		Require(result->endpoint == directory.Endpoint() && result->session &&
					result->generation == runtime.session->Snapshot().generation.value,
				"Attachment identity changed");
		Require(!attachment.Poll(), "Completed attachment ran twice");
		Require(discovery.Snapshot().state == app::RuntimeConnectionState::Disconnected,
				"Completed discovery retained connection");
		completedEpoch = discovery.Snapshot().epoch;
	}
	Require(discovery.Snapshot().epoch == completedEpoch, "Completed attachment disconnected twice");
}

void ExistingEndpointAndChangedProcess()
{
	Directory directory;
	app::RuntimeConnection discovery;
	std::ofstream(directory.Endpoint()) << "pre-existing";
	{
		app::LaunchAttachment attachment{discovery, directory.Profile()};
		attachment.Observe(Running());
		Reject([&] { attachment.Poll(); }, "pre-existing endpoint");
	}
	auto invalidDirectory = directory.Profile();
	invalidDirectory.environment.front().second = directory.Endpoint();
	Reject([&] { app::LaunchAttachment invalid{discovery, invalidDirectory}; }, "Cannot enumerate launch endpoints");
	app::LaunchAttachment attachment{discovery, directory.Profile()};
	attachment.Observe(Running());
	auto changed = Running();
	++changed.process;
	Reject([&] { attachment.Observe(changed); }, "process identity changed");
}

void RecreatedEndpointBeforeSession()
{
	Directory directory;
	app::RuntimeConnection discovery;
	app::LaunchAttachment attachment{discovery, directory.Profile()};
	attachment.Observe(Running());
	RuntimeFixture probing;
	probing.Listen(directory.Endpoint());
	Eventually(
		[&] {
			Require(!attachment.Poll(), "Probing instance attached without a running session");
			return discovery.Snapshot().state == app::RuntimeConnectionState::Connected;
		},
		"Probing instance did not connect");
	const auto firstEpoch = discovery.Snapshot().epoch;
	probing.server.reset();
	Eventually(
		[&] {
			Require(!attachment.Poll(), "Retired probing instance attached");
			return discovery.Snapshot().epoch != firstEpoch;
		},
		"Retired probing instance was not disconnected");
	RuntimeFixture replacement;
	replacement.Begin();
	replacement.Listen(directory.Endpoint());
	std::optional<app::LaunchedSession> result;
	Eventually(
		[&] {
			result = attachment.Poll();
			return result.has_value();
		},
		"Replacement runtime did not attach automatically");
	Require(result->process == Running().process && result->endpoint == directory.Endpoint() &&
				result->generation == replacement.session->Snapshot().generation.value,
			"Replacement attachment lost process or session identity");
	Require(discovery.Snapshot().state == app::RuntimeConnectionState::Disconnected,
			"Replacement attachment retained discovery resources");
}

void ExitWhileRetrying()
{
	Directory directory;
	app::RuntimeConnection discovery;
	{
		app::LaunchAttachment attachment{discovery, directory.Profile()};
		attachment.Observe(Running());
		RuntimeFixture probing;
		probing.Listen(directory.Endpoint());
		Require(!attachment.Poll(), "Discovery attached synchronously");
		probing.server.reset();
		Eventually(
			[&] {
				Require(!attachment.Poll(), "Retired endpoint attached");
				return discovery.Snapshot().state == app::RuntimeConnectionState::Disconnected;
			},
			"Failed startup connection was not released for retry");
		auto exited = Running();
		exited.state = app::LaunchState::Exited;
		Reject([&] { attachment.Observe(exited); }, "ended before attachment");
	}
	Require(discovery.Snapshot().state == app::RuntimeConnectionState::Disconnected,
			"Exiting during discovery retained a connection");
}

void DiscoveryEpochOwnership()
{
	Directory directory;
	RuntimeFixture runtime;
	app::RuntimeConnection discovery;
	std::uint64_t replacementEpoch;
	{
		app::LaunchAttachment attachment{discovery, directory.Profile()};
		runtime.Listen(directory.Endpoint());
		attachment.Observe(Running());
		Require(!attachment.Poll(), "Connecting discovery attached synchronously");
		replacementEpoch = discovery.Connect(directory.Endpoint());
		Reject([&] { attachment.Poll(); }, "connection changed unexpectedly");
	}
	Require(discovery.Snapshot().epoch == replacementEpoch &&
				discovery.Snapshot().state != app::RuntimeConnectionState::Disconnected,
			"Old attachment disconnected a replacement discovery connection");
	{
		app::LaunchAttachment notStarted{discovery, directory.Profile()};
	}
	Require(discovery.Snapshot().epoch == replacementEpoch, "Unstarted attachment changed discovery lifetime");
}

void DiscoveryFailures()
{
	Directory directory;
	const char* prior = std::getenv("SIMXR_DATA_DIR");
	const std::optional<std::string> original = prior ? std::optional<std::string>{prior} : std::nullopt;
	const auto restore = [&] {
		if (original)
			::setenv("SIMXR_DATA_DIR", original->c_str(), 1);
		else
			::unsetenv("SIMXR_DATA_DIR");
	};
	try
	{
		const auto invalid = directory.path / "not-a-directory";
		std::ofstream(invalid) << "file";
		Require(::setenv("SIMXR_DATA_DIR", invalid.c_str(), 1) == 0, "Set discovery fixture");
		Reject([] { app::DiscoverRuntimeEndpoints(); }, "Cannot discover runtime endpoints");
		const auto missing = directory.path / "not-created";
		Require(::setenv("SIMXR_DATA_DIR", missing.c_str(), 1) == 0, "Set missing discovery directory");
		Require(app::DiscoverRuntimeEndpoints().empty(), "A fresh directory should have no runtimes");
	}
	catch (...)
	{
		restore();
		throw;
	}
	restore();
}
} // namespace

int main()
{
	try
	{
		FreshEndpointAndRunningSession();
		ExistingEndpointAndChangedProcess();
		RecreatedEndpointBeforeSession();
		ExitWhileRetrying();
		DiscoveryEpochOwnership();
		DiscoveryFailures();
		std::cout << "PASS explicit launch attachment, exact endpoint/session and discovery epoch ownership\n";
		return 0;
	}
	catch (const std::exception& failure)
	{
		std::cerr << "FAIL " << failure.what() << '\n';
		return 1;
	}
}
