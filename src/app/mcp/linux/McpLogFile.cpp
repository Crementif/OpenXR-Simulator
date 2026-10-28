#include "Pch.h"
#include "app/mcp/McpLogFile.h"

#include <cerrno>
#include <fcntl.h>
#include <sys/stat.h>
#include <system_error>
#include <unistd.h>

namespace simxr::app::mcp
{
std::optional<LogFileSnapshot> ReadLogFile(const std::filesystem::path& path)
{
	const auto fd = ::open(path.c_str(), O_RDONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
	{
		if (errno == ENOENT || errno == ENOTDIR)
			return {};
		throw std::system_error(errno, std::generic_category(), "Cannot open runtime log");
	}
	struct Descriptor { int value; ~Descriptor() { ::close(value); } } descriptor{fd};
	struct stat info{};
	if (::fstat(fd, &info) != 0)
		throw std::system_error(errno, std::generic_category(), "Cannot inspect runtime log");
	if (!S_ISREG(info.st_mode) || info.st_size < 0)
		throw std::runtime_error("Runtime log must be a regular file");
	LogFileSnapshot result;
	result.size = static_cast<std::uint64_t>(info.st_size);
	constexpr std::size_t maximum = 256 * 1024;
	const auto count = static_cast<std::size_t>(std::min<std::uint64_t>(result.size, maximum));
	result.truncated = result.size > count;
	result.bytes.resize(count);
	std::size_t read = 0;
	while (read < count)
	{
		const auto amount = ::pread(fd, result.bytes.data() + read, count - read,
			static_cast<off_t>(result.size - count + read));
		if (amount < 0 && errno == EINTR)
			continue;
		if (amount < 0)
			throw std::system_error(errno, std::generic_category(), "Cannot read runtime log");
		if (!amount)
			break;
		read += static_cast<std::size_t>(amount);
	}
	result.bytes.resize(read);
	if (::fstat(fd, &info) != 0)
		throw std::system_error(errno, std::generic_category(), "Cannot recheck runtime log");
	result.changed = read != count || info.st_size < 0 || static_cast<std::uint64_t>(info.st_size) != result.size;
	return result;
}
bool ClearLogFile(const std::filesystem::path& path)
{
	const auto fd = ::open(path.c_str(), O_WRONLY | O_CLOEXEC | O_NOFOLLOW | O_NONBLOCK);
	if (fd < 0)
	{
		if (errno == ENOENT || errno == ENOTDIR)
			return false;
		throw std::system_error(errno, std::generic_category(), "Cannot open runtime log for clearing");
	}
	struct Descriptor
	{
		int value;
		~Descriptor() { if (value >= 0) ::close(value); }
	} descriptor{fd};
	struct stat info{};
	if (::fstat(fd, &info) != 0)
		throw std::system_error(errno, std::generic_category(), "Cannot inspect runtime log for clearing");
	if (!S_ISREG(info.st_mode) || info.st_nlink != 1)
		throw std::runtime_error("Runtime log must be a regular file with one link to clear it");
	if (::ftruncate(fd, 0) != 0)
		throw std::system_error(errno, std::generic_category(), "Log clear may have completed; cannot truncate runtime log");
	descriptor.value = -1;
	if (::close(fd) != 0)
		throw std::system_error(errno, std::generic_category(), "Log clear completed, but closing the file failed");
	return true;
}

}
