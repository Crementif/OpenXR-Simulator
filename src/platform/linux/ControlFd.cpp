#include "platform/linux/ControlFd.h"
#include "Pch.h"

#include <fcntl.h>
#include <unistd.h>

namespace simxr::control
{
struct ControlFdGuard
{
	int value;
	~ControlFdGuard()
	{
		if (value >= 0)
		{
			const auto saved = errno;
			close(value); // Linux closes on EINTR; retry could close a reused FD.
			errno = saved;
		}
	}
};
static void CloseOnExec(int fd)
{
	int flags;
	do
	{
		flags = fcntl(fd, F_GETFD);
	} while (flags < 0 && errno == EINTR);
	if (flags < 0)
		throw std::system_error(errno, std::generic_category(), "Query control FD flags");
	int result;
	do
	{
		result = fcntl(fd, F_SETFD, flags | FD_CLOEXEC);
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		throw std::system_error(errno, std::generic_category(), "Set control FD close-on-exec");
}
class OwnedControlFd final : public TransferHandle
{
  public:
	explicit OwnedControlFd(int fd) noexcept : m_fd(fd) {}
	~OwnedControlFd() override
	{
		ControlFdGuard cleanup{m_fd};
	}
	TransferHandleKind Kind() const noexcept override
	{
		return TransferHandleKind::OpaqueFd;
	}
	std::intptr_t Value() const noexcept override
	{
		return m_fd;
	}
	std::shared_ptr<TransferHandle> Duplicate() const override
	{
		return DuplicateControlFd(m_fd);
	}

  private:
	const int m_fd;
};
std::shared_ptr<TransferHandle> OwnControlFd(int fd)
{
	ControlFdGuard pending{fd};
	if (fd < 0)
		throw std::invalid_argument("Control FD must be a valid owned descriptor");
	CloseOnExec(fd);
	auto result = std::make_shared<OwnedControlFd>(fd);
	pending.value = -1;
	return result;
}
std::shared_ptr<TransferHandle> DuplicateControlFd(int fd)
{
	if (fd < 0)
		throw std::invalid_argument("Cannot duplicate an invalid control FD");
	int duplicate;
	do
	{
		duplicate = fcntl(fd, F_DUPFD_CLOEXEC, 0);
	} while (duplicate < 0 && errno == EINTR);
	if (duplicate < 0)
		throw std::system_error(errno, std::generic_category(), "Duplicate control FD");
	return OwnControlFd(duplicate);
}
} // namespace simxr::control
