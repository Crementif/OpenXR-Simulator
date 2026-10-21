#include <simxr/game_bridge.h>

#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <dlfcn.h>
#include <filesystem>
#include <stdexcept>
#include <string>
#include <thread>

int main(int argc, char** argv)
{
	void* library = nullptr;
	try
	{
		if (argc != 4) throw std::invalid_argument("Expected runtime library, endpoint and stop marker");
		if (setenv("SIMXR_CONTROL_SOCKET", argv[2], 1) != 0) throw std::runtime_error("Set endpoint environment failed");
		library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
		if (!library) throw std::runtime_error(dlerror());
		const auto getApi = reinterpret_cast<PFN_simxrGetGameBridgeApi>(dlsym(library, "simxrGetGameBridgeApi"));
		SimxrGameBridgeApi api{}; api.size = sizeof(api);
		if (!getApi || !getApi(1, &api)) throw std::runtime_error("Native game bridge negotiation failed");
		const auto game = api.registerGame(api.context, SIMXR_GAME_CAN_RESET);
		if (!game) throw std::runtime_error("Native game bridge registration failed");
		std::puts("READY game safe-update fixture; no XR instance or graphics device"); std::fflush(stdout);
		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{10};
		unsigned resets = 0;
		while (!std::filesystem::exists(argv[3]))
		{
			if (std::chrono::steady_clock::now() >= deadline) throw std::runtime_error("Game fixture timed out");
			SimxrGameCommand command{};
			if (api.poll(api.context, game, &command))
			{
				if (command.kind != SIMXR_GAME_RESET || std::string_view{command.payload, command.payloadLength} != "checkpoint:start")
					throw std::runtime_error("Game received unexpected command");
				++resets;
				const std::string telemetry = "{\"reset_count\":" + std::to_string(resets) + "}";
				if (!api.publish(api.context, game, telemetry.data(), telemetry.size()) ||
					!api.complete(api.context, game, command.id, 1, "reset applied", 13))
					throw std::runtime_error("Actual game acknowledgement failed");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds{1});
		}
		if (resets != 1) throw std::runtime_error("Idempotent reset executed a different number of times");
		api.unregisterGame(api.context, game);
		if (!api.shutdown(api.context)) throw std::runtime_error("Explicit runtime shutdown failed");
		dlclose(library); library = nullptr;
		std::puts("PASS native optional C ABI, game update acknowledgement and shutdown without XR/GPU"); return 0;
	}
	catch (const std::exception& error)
	{
		// Do not unload a live runtime on an exceptional fixture path: its server
		// threads still own callbacks. Process exit reclaims this failed fixture.
		std::fprintf(stderr, "FAIL %s\n", error.what()); return 1;
	}
}
