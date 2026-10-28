#include "Pch.h"
#include "app/platform/FileStorage.h"
#include "app/services/CompanionSettings.h"

#include <cerrno>
#include <cstdlib>
#include <fcntl.h>
#include <linux/fs.h>
#include <sys/syscall.h>
#include <sys/file.h>
#include <stdexcept>
#include <system_error>
#include <unistd.h>

namespace simxr::app
{
SettingsFileLock::SettingsFileLock(const std::filesystem::path& path)
{
	if (!path.is_absolute())
		throw std::invalid_argument("Companion settings destination must be absolute");
	std::filesystem::create_directories(path.parent_path());
	auto lockPath = path;
	lockPath += ".lock";
	const int descriptor = ::open(lockPath.c_str(), O_CREAT | O_RDWR | O_CLOEXEC, 0600);
	if (descriptor < 0)
		throw std::system_error(errno, std::generic_category(), "Cannot open settings lock");
	while (::flock(descriptor, LOCK_EX) != 0)
	{
		if (errno == EINTR)
			continue;
		const auto error = errno;
		::close(descriptor);
		throw std::system_error(error, std::generic_category(), "Cannot lock settings");
	}
	m_handle = descriptor;
}
SettingsFileLock::~SettingsFileLock()
{
	if (m_handle >= 0)
		::close(static_cast<int>(m_handle));
}
static std::filesystem::path Absolute(const char* value)
{
	std::filesystem::path path(value);
	if (!path.is_absolute())
		throw std::runtime_error("Companion configuration path must be absolute");
	return path;
}
std::filesystem::path CompanionSettingsPath()
{
	if (const auto path = std::getenv("SIMXR_COMPANION_SETTINGS"); path && *path)
		return Absolute(path);
	if (const auto path = std::getenv("XDG_CONFIG_HOME"); path && *path)
		return Absolute(path) / "OpenXR-Simulator/companion.settings";
	if (const auto path = std::getenv("HOME"); path && *path)
		return Absolute(path) / ".config/OpenXR-Simulator/companion.settings";
	throw std::runtime_error("No absolute companion configuration directory is available");
}
std::filesystem::path CreatePrivateTemporaryFile(const std::filesystem::path& destination)
{
	auto pattern = destination.native() + ".XXXXXX";
	const int descriptor = ::mkstemp(pattern.data());
	if (descriptor < 0)
		throw std::system_error(errno, std::generic_category(), "Cannot create private temporary file");
	const int closed = ::close(descriptor);
	const int closeError = errno;
	try
	{
		if (closed != 0)
			throw std::system_error(closeError, std::generic_category(), "Cannot close temporary file");
		return std::filesystem::path(pattern);
	}
	catch (...)
	{
		::unlink(pattern.c_str());
		throw;
	}
}
void CommitTemporaryFile(const std::filesystem::path& temporary, const std::filesystem::path& destination)
{
	std::filesystem::rename(temporary, destination);
}
bool CanExecuteFile(const std::filesystem::path& path)
{
	return ::access(path.c_str(), X_OK) == 0;
}
void RenameFileWithoutReplacing(const std::filesystem::path& source, const std::filesystem::path& destination)
{
	if (::syscall(SYS_renameat2, AT_FDCWD, source.c_str(), AT_FDCWD, destination.c_str(), RENAME_NOREPLACE) != 0)
		throw std::system_error(errno, std::generic_category(), "Cannot rename file without replacing another file");
}
}
