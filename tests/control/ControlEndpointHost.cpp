#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "runtime/Runtime.h"

#include <chrono>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <stdexcept>
#include <thread>

using namespace simxr;
using namespace std::chrono_literals;

int main(int argc, char** argv)
{
	try
	{
		if (argc != 3 && argc != 4)
			throw std::invalid_argument("Expected socket, application-start marker and optional finish marker paths");
		Runtime runtime{ExtensionRegistry{std::span<const ExtensionDefinition>{}}};
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		std::strcpy(create.applicationInfo.applicationName, "CPU control integration host");
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
		Runtime::InstanceRef instance;
		Runtime::SessionRef session;
		if (runtime.CreateInstance(&create, instance) != XR_SUCCESS ||
			runtime.CreateSession(instance, session) != XR_SUCCESS ||
			session->ConfigureCapturePool(FramePool{8}, true) != XR_SUCCESS)
			throw std::runtime_error("Create CPU host failed");
		FrameSchedulerConfig timing;
		timing.mode = ExecutionMode::Deterministic;
		XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		if (session->Ready(timing.epoch) != XR_SUCCESS || session->Begin(begin, timing) != XR_SUCCESS)
			throw std::runtime_error("Begin CPU host failed");
		control::ControlService service{runtime};
		control::LocalControlServer server{service, argv[1]};
		std::puts("READY CPU fixture; no native GPU qualification");
		std::fflush(stdout);
		const auto deadline = std::chrono::steady_clock::now() + 15s;
		while (!std::filesystem::exists(argv[2]))
		{
			if (std::chrono::steady_clock::now() > deadline)
				throw std::runtime_error("Application-start marker timed out");
			std::this_thread::sleep_for(1ms);
		}
		for (int index = 0; index < 6; ++index)
		{
			FramePrediction prediction;
			if (session->WaitFrame(prediction) != XR_SUCCESS || session->BeginFrame() != XR_SUCCESS ||
				session->EndFrame(prediction) != XR_SUCCESS)
				throw std::runtime_error("CPU host frame failed");
		}
		while (argc == 4 && !std::filesystem::exists(argv[3]))
		{
			if (std::chrono::steady_clock::now() > deadline)
				throw std::runtime_error("Application-finish marker timed out");
			std::this_thread::sleep_for(1ms);
		}
		server.Close();
		service.Close();
		runtime.Close();
		std::puts("PASS CPU control endpoint lifecycle");
		return 0;
	}
	catch (const std::exception& error)
	{
		std::fprintf(stderr, "FAIL: %s\n", error.what());
		return 1;
	}
}
