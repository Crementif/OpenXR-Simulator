#include "Pch.h"
#include "platform/linux/InstallUse.h"

#include <dlfcn.h>
#include <fcntl.h>
#include <sys/file.h>
#include <sys/stat.h>
#include <sys/sysmacros.h>
#include <unistd.h>

#include <cstdio>
#include <filesystem>
#include <fstream>

namespace simxr
{
static void InstallAnchor() {}

class __attribute__((visibility("hidden"))) InstallUse
{
  public:
	InstallUse() noexcept;
	~InstallUse();
	bool Ready() const noexcept
	{
		return m_ready;
	}

  private:
	int m_file = -1;
	bool m_ready = false;
};

InstallUse::InstallUse() noexcept
{
	try
	{
		Dl_info info{};
		if (!dladdr(reinterpret_cast<const void*>(&InstallAnchor), &info) || !info.dli_fname)
			return;
		const auto binary = std::filesystem::canonical(info.dli_fname);
		const auto root = binary.parent_path().parent_path();
		const auto marker = root / ".simxr-install";
		std::error_code error;
		if (!std::filesystem::exists(marker, error))
		{
			m_ready = !error;
			return;
		}
		m_file = open((root / ".simxr-use-lock").c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW);
		if (m_file < 0 || flock(m_file, LOCK_SH | LOCK_NB) != 0)
			return;

		// A program may have mapped an older binary just before an upgrade.
		// Confirm its mapped inode still belongs to the current installation.
		struct stat current{};
		if (stat(binary.c_str(), &current) != 0)
			return;
		std::ifstream maps("/proc/self/maps");
		std::string line;
		const auto address = reinterpret_cast<std::uintptr_t>(&InstallAnchor);
		while (std::getline(maps, line))
		{
			unsigned long long start = 0, end = 0, inode = 0;
			unsigned deviceMajor = 0, deviceMinor = 0;
			if (std::sscanf(line.c_str(), "%llx-%llx %*s %*s %x:%x %llu", &start, &end,
							&deviceMajor, &deviceMinor, &inode) != 5 || address < start || address >= end)
				continue;
			m_ready = inode == current.st_ino && deviceMajor == major(current.st_dev) &&
					  deviceMinor == minor(current.st_dev);
			return;
		}
	}
	catch (...)
	{
	}
}
InstallUse::~InstallUse()
{
	if (m_file >= 0)
		close(m_file);
}

static InstallUse s_installUse;
bool InstallationReady() noexcept
{
	return s_installUse.Ready();
}
}
