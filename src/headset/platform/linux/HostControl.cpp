#include "headset/HostControl.h"
#include "Pch.h"

#include <poll.h>
#include <sys/stat.h>
#include <unistd.h>

namespace simxr::headset
{
void ValidateHostControlPipe()
{
	struct stat status{};
	if (fstat(STDIN_FILENO, &status) != 0 || !S_ISFIFO(status.st_mode))
		throw std::invalid_argument("Parent control requires a standard-input pipe");
}
bool HostStopRequested()
{
	pollfd descriptor{STDIN_FILENO, POLLIN, 0};
	const auto result = poll(&descriptor, 1, 0);
	if (result < 0)
	{
		if (errno == EINTR)
			return false;
		throw std::runtime_error("Cannot poll parent control pipe");
	}
	return result > 0 && (descriptor.revents & (POLLIN | POLLHUP | POLLERR | POLLNVAL));
}
} // namespace simxr::headset
