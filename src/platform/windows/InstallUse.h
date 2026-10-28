#pragma once

#include <windows.h>

namespace simxr::windows
{
bool AcquireInstallationUse(HMODULE module, HANDLE& handle) noexcept;

class InstallUse
{
  public:
	InstallUse() noexcept;
	~InstallUse();
	InstallUse(const InstallUse&) = delete;
	InstallUse& operator=(const InstallUse&) = delete;
	bool Ready() const noexcept;

  private:
	HANDLE m_handle = INVALID_HANDLE_VALUE;
	bool m_ready = false;
};
}
