#include "Pch.h"
#include "platform/windows/InstallUse.h"

namespace simxr::windows
{
static HANDLE s_runtimeUse = INVALID_HANDLE_VALUE;
}

BOOL WINAPI DllMain(HINSTANCE module, DWORD reason, LPVOID)
{
	if (reason == DLL_PROCESS_ATTACH)
		return simxr::windows::AcquireInstallationUse(module, simxr::windows::s_runtimeUse) ? TRUE : FALSE;
	if (reason == DLL_PROCESS_DETACH && simxr::windows::s_runtimeUse != INVALID_HANDLE_VALUE)
		CloseHandle(simxr::windows::s_runtimeUse);
	return TRUE;
}
