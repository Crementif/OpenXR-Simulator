#include "Pch.h"
#include "ProbeEnvironment.h"

#include <simxr/game_bridge.h>

#include <cstdio>
#include <cstdlib>
#if defined(_WIN32)
#include <windows.h>
#else
#include <dlfcn.h>
#endif
#include <filesystem>
#include <stdexcept>
#include <thread>

#if defined(_WIN32)
static bool s_closeRequested = false;
static LRESULT CALLBACK GameWindowProc(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_CLOSE)
	{
		s_closeRequested = true;
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}
int wmain(int argc, wchar_t** argv)
#else
int main(int argc, char** argv)
#endif
{
#if defined(_WIN32)
	HMODULE library = nullptr;
#else
	void* library = nullptr;
#endif
	try
	{
		const auto timeoutSeconds = simxr::probe::EnvironmentCount("SIMXR_GAME_PROBE_SECONDS", 10, 1, 90);
		if (argc != 4)
			throw std::invalid_argument("Expected runtime library, endpoint and stop marker");
#if defined(_WIN32)
		if (!SetEnvironmentVariableW(L"SIMXR_CONTROL_SOCKET", argv[2]))
			throw std::runtime_error("Set endpoint environment failed");
		library = LoadLibraryW(argv[1]);
		if (!library)
			throw std::runtime_error("Cannot load runtime library: " + std::to_string(GetLastError()));
		const auto getApi = reinterpret_cast<PFN_simxrGetGameBridgeApi>(GetProcAddress(library, "simxrGetGameBridgeApi"));
#else
		if (setenv("SIMXR_CONTROL_SOCKET", argv[2], 1) != 0)
			throw std::runtime_error("Set endpoint environment failed");
		library = dlopen(argv[1], RTLD_NOW | RTLD_LOCAL);
		if (!library)
			throw std::runtime_error(dlerror());
		const auto getApi = reinterpret_cast<PFN_simxrGetGameBridgeApi>(dlsym(library, "simxrGetGameBridgeApi"));
#endif
		SimxrGameBridgeApi api{};
		api.size = sizeof(api);
		if (!getApi || !getApi(SIMXR_GAME_BRIDGE_VERSION, &api) || api.version != SIMXR_GAME_BRIDGE_VERSION ||
			api.size < sizeof(api) || !api.registerGame || !api.unregisterGame || !api.poll || !api.complete ||
			!api.publish || !api.shutdown)
			throw std::runtime_error("Native game bridge negotiation failed");
		const auto game = api.registerGame(api.context, SIMXR_GAME_CAN_RESET);
		if (!game)
			throw std::runtime_error("Native game bridge registration failed");
		std::puts("READY game safe-update fixture; no XR instance or graphics device");
		std::fflush(stdout);

		const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{timeoutSeconds};
		unsigned resets = 0;
#if defined(_WIN32)
		WNDCLASSW windowClass{};
		windowClass.lpfnWndProc = GameWindowProc;
		windowClass.hInstance = GetModuleHandleW(nullptr);
		windowClass.lpszClassName = L"SimxrGameProbeWindow";
		if (!RegisterClassW(&windowClass))
			throw std::runtime_error("Cannot register game probe window");
		const auto window = CreateWindowExW(0, windowClass.lpszClassName, L"SimXR Game Probe", WS_OVERLAPPED,
			CW_USEDEFAULT, CW_USEDEFAULT, 100, 100, nullptr, nullptr, windowClass.hInstance, nullptr);
		if (!window)
			throw std::runtime_error("Cannot create game probe window");
#endif
		while (!std::filesystem::exists(std::filesystem::path{argv[3]})
#if defined(_WIN32)
			   && !s_closeRequested
#endif
		)
		{
#if defined(_WIN32)
			MSG message{};
			while (PeekMessageW(&message, nullptr, 0, 0, PM_REMOVE))
			{
				TranslateMessage(&message);
				DispatchMessageW(&message);
			}
			if (s_closeRequested)
				break;
#endif
			if (std::chrono::steady_clock::now() >= deadline)
				throw std::runtime_error("Game fixture timed out");
			SimxrGameCommand command{};
			if (api.poll(api.context, game, &command))
			{
				if (command.kind != SIMXR_GAME_RESET || command.payloadLength > sizeof(command.payload) ||
					std::string_view{command.payload, command.payloadLength} != "checkpoint:start")
					throw std::runtime_error("Game received unexpected command");
				++resets;
				const std::string telemetry = "{\"reset_count\":" + std::to_string(resets) + "}";
				if (!api.publish(api.context, game, telemetry.data(), static_cast<std::uint32_t>(telemetry.size())) ||
					!api.complete(api.context, game, command.id, 1, "reset applied", 13))
					throw std::runtime_error("Actual game acknowledgement failed");
			}
			std::this_thread::sleep_for(std::chrono::milliseconds{1});
		}
		if (resets != 1)
			throw std::runtime_error("Idempotent reset executed a different number of times");
		api.unregisterGame(api.context, game);
		if (!api.shutdown(api.context))
			throw std::runtime_error("Explicit runtime shutdown failed");
#if defined(_WIN32)
		DestroyWindow(window);
		if (!FreeLibrary(library))
			throw std::runtime_error("Cannot unload runtime library");
#else
		if (dlclose(library) != 0)
			throw std::runtime_error(dlerror());
#endif
		library = nullptr;
		std::puts("PASS native optional C ABI, game update acknowledgement and shutdown without XR/GPU");
		return 0;
	}
	catch (const std::exception& error)
	{
		// Do not unload a live runtime on an exceptional fixture path: its server
		// threads still own callbacks. Process exit reclaims this failed fixture.
		std::fprintf(stderr, "FAIL %s\n", error.what());
		return 1;
	}
}
