#include "Pch.h"
#include "control/ControlService.h"
#include "control/LocalControlServer.h"
#include "control/RuntimeEndpoint.h"
#include "runtime/Runtime.h"

#include <windows.h>

struct ControlFixture
{
	simxr::Runtime runtime{simxr::ExtensionRegistry{std::span<const simxr::ExtensionDefinition>{}}};
	simxr::Runtime::InstanceRef instance;
	simxr::Runtime::SessionRef session;
	simxr::control::ControlService service{runtime};
	std::unique_ptr<simxr::control::LocalControlServer> server;

	ControlFixture()
	{
		XrInstanceCreateInfo create{XR_TYPE_INSTANCE_CREATE_INFO};
		create.applicationInfo.apiVersion = XR_MAKE_VERSION(1, 0, 27);
		std::strcpy(create.applicationInfo.applicationName, "MCP restart fixture");
		if (runtime.CreateInstance(&create, instance) != XR_SUCCESS ||
			runtime.CreateSession(instance, session) != XR_SUCCESS)
			throw std::runtime_error("Create fixture runtime session failed");
		simxr::FrameSchedulerConfig timing;
		XrSessionBeginInfo begin{XR_TYPE_SESSION_BEGIN_INFO, nullptr, XR_VIEW_CONFIGURATION_TYPE_PRIMARY_STEREO};
		if (session->Ready(timing.epoch) != XR_SUCCESS || session->Begin(begin, timing) != XR_SUCCESS)
			throw std::runtime_error("Begin fixture runtime session failed");
		server = std::make_unique<simxr::control::LocalControlServer>(
			service, simxr::control::RuntimeEndpoint(GetCurrentProcessId()));
	}
};

static LRESULT CALLBACK WindowProcedure(HWND window, UINT message, WPARAM wparam, LPARAM lparam)
{
	if (message == WM_DESTROY)
	{
		PostQuitMessage(0);
		return 0;
	}
	return DefWindowProcW(window, message, wparam, lparam);
}

int wmain(int argc, wchar_t** argv)
{
	const auto instance = GetModuleHandleW(nullptr);
	WNDCLASSW windowClass{};
	windowClass.lpfnWndProc = WindowProcedure;
	windowClass.hInstance = instance;
	windowClass.lpszClassName = L"SimxrRestartTest";
	if (!RegisterClassW(&windowClass))
		return 1;
	const auto window = CreateWindowExW(0, windowClass.lpszClassName, L"Simxr restart test", WS_OVERLAPPEDWINDOW,
										CW_USEDEFAULT, CW_USEDEFAULT, 240, 120, nullptr, nullptr, instance, nullptr);
	if (!window)
		return 2;
	ControlFixture control;
	if (argc > 1)
	{
		std::ofstream marker(std::filesystem::path(argv[1]), std::ios::app);
		marker << "ready\n";
		marker.close();
		if (!marker)
			return 3;
	}
	MSG message{};
	while (GetMessageW(&message, nullptr, 0, 0) > 0)
	{
		TranslateMessage(&message);
		DispatchMessageW(&message);
	}
	return 0;
}
