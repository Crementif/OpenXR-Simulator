#include "Pch.h"
#include "platform/ProcessIdentity.h"
#include "platform/windows/ModuleReference.h"

#include <windows.h>

namespace simxr::test
{
static std::optional<windows::ModuleReference> s_reference;

extern "C" __declspec(dllexport) std::uint64_t allocateProcessIdentity() noexcept
{
	try
	{
		return AllocateProcessIdentity();
	}
	catch (...)
	{
		return 0;
	}
}

extern "C" __declspec(dllexport) BOOL holdModuleReference() noexcept
{
	try
	{
		if (s_reference)
			return FALSE;
		s_reference.emplace();
		return TRUE;
	}
	catch (...)
	{
		return FALSE;
	}
}

extern "C" __declspec(dllexport) BOOL releaseModuleReference() noexcept
{
	if (!s_reference)
		return FALSE;
	// The executable holds a separate reference until this call returns.
	s_reference.reset();
	return TRUE;
}
} // namespace simxr::test
