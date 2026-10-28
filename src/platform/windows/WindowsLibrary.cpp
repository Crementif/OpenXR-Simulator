#include "platform/windows/WindowsLibrary.h"
#include "Pch.h"

namespace simxr::windows
{
WindowsLibrary::WindowsLibrary(const wchar_t* name)
{
	m_module = LoadLibraryExW(name, nullptr, LOAD_LIBRARY_SEARCH_SYSTEM32);
	if (!m_module)
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
								"Load system graphics library");
}

WindowsLibrary::~WindowsLibrary() noexcept
{
	if (m_module && !FreeLibrary(m_module))
		std::fprintf(stderr, "SimXR: failed to release graphics library (error %lu)\n", GetLastError());
}

FARPROC WindowsLibrary::Procedure(const char* name) const
{
	const auto procedure = GetProcAddress(m_module, name);
	if (!procedure)
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(),
								std::string("Resolve graphics entry point ") + name);
	return procedure;
}
} // namespace simxr::windows
