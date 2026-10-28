#include "platform/windows/ModuleReference.h"
#include "Pch.h"

#include <windows.h>

namespace simxr::windows
{
ModuleReference::ModuleReference()
{
	static const wchar_t s_anchor = L'\0';
	HMODULE module = nullptr;
	if (!GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS, &s_anchor, &module))
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Retain runtime module");
	m_module = module;
}

ModuleReference::~ModuleReference() noexcept
{
	Release();
}

ModuleReference::ModuleReference(ModuleReference&& other) noexcept : m_module(std::exchange(other.m_module, nullptr)) {}

ModuleReference& ModuleReference::operator=(ModuleReference&& other) noexcept
{
	if (this != &other)
	{
		Release();
		m_module = std::exchange(other.m_module, nullptr);
	}
	return *this;
}

void ModuleReference::Release() noexcept
{
	const auto module = static_cast<HMODULE>(std::exchange(m_module, nullptr));
	if (module && !FreeLibrary(module))
		std::fprintf(stderr, "SimXR: failed to release runtime module reference (error %lu)\n", GetLastError());
}
} // namespace simxr::windows
