#pragma once

#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>

namespace simxr::windows
{
// Declare before native objects and resolved entry points that depend on this library.
class WindowsLibrary final
{
public:
	explicit WindowsLibrary(const wchar_t* name);
	~WindowsLibrary() noexcept;
	WindowsLibrary(const WindowsLibrary&) = delete;
	WindowsLibrary& operator=(const WindowsLibrary&) = delete;
	FARPROC Procedure(const char* name) const;

private:
	HMODULE m_module = nullptr;
};
} // namespace simxr::windows
