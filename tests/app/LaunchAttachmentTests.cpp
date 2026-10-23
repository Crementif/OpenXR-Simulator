#include "../TestSupport.h"
#include "app/services/LaunchAttachment.h"
#include "app/RuntimeConnection.h"
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
		profile.environment = {{"SIMXR_DATA_DIR", path.string()}, {"SIMXR_CONTROL_SOCKET", ""}};
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
template <class Work> void Reject(Work&& work, std::string_view reason)
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
			[&]
			{
				Require(!attachment.Poll(), "Non-running session attached");
				return discovery.Snapshot().state == app::RuntimeConnectionState::Connected;
			},
			"Discovery did not connect asynchronously");
		runtime.Begin();
		std::optional<app::LaunchedSession> result;
		Eventually(
			[&]
			{
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
		Reject(
			[&]
			{
				attachment.Poll();
			},
			"pre-existing endpoint");
	}
	auto invalidDirectory = directory.Profile();
	invalidDirectory.environment.front().second = directory.Endpoint();
	Reject(
		[&]
		{
			app::LaunchAttachment invalid{discovery, invalidDirectory};
		},
		"Cannot enumerate launch endpoints");
	auto custom = directory.Profile();
	custom.environment.back().second = directory.Endpoint();
	Reject(
		[&]
		{
			app::LaunchAttachment attachment{discovery, custom};
		},
		"new absolute local control endpoint");
	app::LaunchAttachment attachment{discovery, directory.Profile()};
	attachment.Observe(Running());
	auto changed = Running();
	++changed.process;
	Reject(
		[&]
		{
			attachment.Observe(changed);
		},
		"process identity changed");
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
		Reject(
			[&]
			{
				attachment.Poll();
			},
			"connection changed unexpectedly");
	}
	Require(discovery.Snapshot().epoch == replacementEpoch &&
				discovery.Snapshot().state != app::RuntimeConnectionState::Disconnected,
			"Old attachment disconnected a replacement discovery connection");
	{
		app::LaunchAttachment notStarted{discovery, directory.Profile()};
	}
	Require(discovery.Snapshot().epoch == replacementEpoch, "Unstarted attachment changed discovery lifetime");
}
} // namespace
int main()
{
	try
	{
		FreshEndpointAndRunningSession();
		ExistingEndpointAndChangedProcess();
		DiscoveryEpochOwnership();
		std::cout << "PASS explicit launch attachment, exact endpoint/session and discovery epoch ownership\n";
		return 0;
	}
	catch (const std::exception& failure)
	{
		std::cerr << "FAIL " << failure.what() << '\n';
		return 1;
	}
}
