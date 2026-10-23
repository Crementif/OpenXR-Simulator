#include "LinuxFdChannel.h"
#include "Pch.h"

#include <cerrno>
#include <cstring>
#include <limits>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/uio.h>
#include <system_error>
#include <unistd.h>

namespace sharing
{
Descriptors::~Descriptors()
{
	for (int value : values)
		if (value >= 0)
			close(value);
}

int Descriptors::Take(std::size_t index)
{
	return std::exchange(values[index], -1);
}

void Send(int socket, const void* data, std::size_t size, std::span<const int> descriptors)
{
	if (descriptors.size() > 3 || size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()))
		throw std::invalid_argument("Sharing packet exceeds its byte or descriptor bound");
	iovec vector{const_cast<void*>(data), size};
	alignas(cmsghdr) std::array<char, CMSG_SPACE(3 * sizeof(int))> ancillary{};
	msghdr message{};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	if (!descriptors.empty())
	{
		message.msg_control = ancillary.data();
		message.msg_controllen = CMSG_SPACE(descriptors.size_bytes());
		auto control = CMSG_FIRSTHDR(&message);
		control->cmsg_level = SOL_SOCKET;
		control->cmsg_type = SCM_RIGHTS;
		control->cmsg_len = CMSG_LEN(descriptors.size_bytes());
		std::memcpy(CMSG_DATA(control), descriptors.data(), descriptors.size_bytes());
	}
	ssize_t sent;
	do
	{
		sent = sendmsg(socket, &message, MSG_NOSIGNAL);
	} while (sent < 0 && errno == EINTR);
	if (sent < 0)
		throw std::system_error(errno, std::generic_category(), "Send sharing packet");
	if (sent != static_cast<ssize_t>(size))
		throw std::runtime_error("Sharing channel sent an incomplete packet");
}

void Receive(int socket, void* data, std::size_t size, Descriptors& descriptors, std::size_t expectedDescriptors)
{
	if (expectedDescriptors > descriptors.values.size() ||
		size > static_cast<std::size_t>(std::numeric_limits<ssize_t>::max()))
		throw std::invalid_argument("Sharing packet exceeds its byte or descriptor bound");
	for (int descriptor : descriptors.values)
		if (descriptor >= 0)
			throw std::invalid_argument("Sharing receive requires empty descriptor ownership");
	pollfd readiness{socket, POLLIN, 0};
	const auto deadline = std::chrono::steady_clock::now() + std::chrono::seconds{5};
	int result = 0;
	do
	{
		const auto remaining =
			std::chrono::ceil<std::chrono::milliseconds>(deadline - std::chrono::steady_clock::now());
		if (remaining.count() <= 0)
		{
			result = 0;
			break;
		}
		result = poll(&readiness, 1, static_cast<int>(remaining.count()));
	} while (result < 0 && errno == EINTR);
	if (result < 0)
		throw std::system_error(errno, std::generic_category(), "Wait for sharing packet");
	if (result != 1)
		throw std::runtime_error("Sharing channel timed out (5 seconds)");
	iovec vector{data, size};
	alignas(cmsghdr) std::array<char, CMSG_SPACE(3 * sizeof(int))> ancillary{};
	msghdr message{};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = ancillary.data();
	message.msg_controllen = ancillary.size();
	ssize_t received;
	do
	{
		received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
	} while (received < 0 && errno == EINTR);
	if (received < 0)
		throw std::system_error(errno, std::generic_category(), "Receive sharing packet");
	std::size_t count = 0;
	for (auto control = CMSG_FIRSTHDR(&message); control; control = CMSG_NXTHDR(&message, control))
	{
		if (control->cmsg_level != SOL_SOCKET || control->cmsg_type != SCM_RIGHTS || control->cmsg_len < CMSG_LEN(0))
			continue;
		const auto payload = control->cmsg_len - CMSG_LEN(0);
		for (std::size_t offset = 0; offset + sizeof(int) <= payload; offset += sizeof(int))
		{
			int descriptor;
			std::memcpy(&descriptor, CMSG_DATA(control) + offset, sizeof(int));
			if (count < descriptors.values.size())
				descriptors.values[count] = descriptor;
			else
				close(descriptor);
			++count;
		}
	}
	if (received != static_cast<ssize_t>(size) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) ||
		count != expectedDescriptors)
		throw std::runtime_error("Sharing channel closed or returned an invalid packet");
}
} // namespace sharing
