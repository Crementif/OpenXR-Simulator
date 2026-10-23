#include "Pch.h"
#include "platform/ProcessIdentity.h"

#include <system_error>
#include <windows.h>

namespace simxr
{
std::uint64_t AllocateProcessIdentity()
{
	LUID identity{};
	if (!AllocateLocallyUniqueId(&identity))
		throw std::system_error(static_cast<int>(GetLastError()), std::system_category(), "Allocate process identity");
	// Windows keeps these nonzero identities unique across runtime DLL reloads.
	return (static_cast<std::uint64_t>(static_cast<std::uint32_t>(identity.HighPart)) << 32) | identity.LowPart;
}
} // namespace simxr
