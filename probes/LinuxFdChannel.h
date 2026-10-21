#pragma once

#include <array>
#include <cerrno>
#include <cstring>
#include <stdexcept>
#include <span>
#include <utility>
#include <poll.h>
#include <sys/socket.h>
#include <unistd.h>

namespace sharing
{
struct Descriptors
{
	std::array<int, 3> values{-1, -1, -1};
	Descriptors() = default;
	Descriptors(const Descriptors&) = delete;
	Descriptors& operator=(const Descriptors&) = delete;
	~Descriptors()
	{
		for (int value : values)
			if (value >= 0) close(value);
	}
	int Take(std::size_t index) { return std::exchange(values[index], -1); }
};

inline void Send(int socket, const void* data, std::size_t size, std::span<const int> descriptors = {})
{
	if (descriptors.size() > 3) throw std::runtime_error("Too many sharing descriptors");
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
	do { sent = sendmsg(socket, &message, MSG_NOSIGNAL); } while (sent < 0 && errno == EINTR);
	if (sent != static_cast<ssize_t>(size)) throw std::runtime_error("Sharing channel send failed");
}

inline void Receive(int socket, void* data, std::size_t size, Descriptors& descriptors, std::size_t expectedDescriptors = 0)
{
	for (int descriptor : descriptors.values)
		if (descriptor >= 0) throw std::runtime_error("Sharing receive requires empty descriptor ownership");
	pollfd readiness{socket, POLLIN, 0};
	int result;
	do { result = poll(&readiness, 1, 5000); } while (result < 0 && errno == EINTR);
	if (result != 1) throw std::runtime_error("Sharing channel timed out (5 seconds)");
	iovec vector{data, size};
	alignas(cmsghdr) std::array<char, CMSG_SPACE(3 * sizeof(int))> ancillary{};
	msghdr message{};
	message.msg_iov = &vector;
	message.msg_iovlen = 1;
	message.msg_control = ancillary.data();
	message.msg_controllen = ancillary.size();
	ssize_t received;
	do { received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC); } while (received < 0 && errno == EINTR);
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
			if (count < descriptors.values.size()) descriptors.values[count] = descriptor;
			else close(descriptor);
			++count;
		}
	}
	if (received != static_cast<ssize_t>(size) || (message.msg_flags & (MSG_TRUNC | MSG_CTRUNC)) || count != expectedDescriptors)
		throw std::runtime_error("Sharing channel closed or returned an invalid packet");
}
}
