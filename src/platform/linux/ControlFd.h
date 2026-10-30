#pragma once

#include "control/ControlPacket.h"

#include <memory>
#include <string>
#include <sys/socket.h>
#include <sys/un.h>

namespace simxr::control
{
// Takes ownership immediately, including every exceptional path.
std::shared_ptr<TransferHandle> OwnControlFd(int fd);
// Does not consume the caller's descriptor; the duplicate is close-on-exec.
std::shared_ptr<TransferHandle> DuplicateControlFd(int fd);

// Socket paths longer than sun_path are reached through an open handle on their directory.
class ControlSocketAddress
{
public:
	explicit ControlSocketAddress(const std::string& path);
	~ControlSocketAddress();
	ControlSocketAddress(const ControlSocketAddress&) = delete;
	ControlSocketAddress& operator=(const ControlSocketAddress&) = delete;

	const sockaddr* Get() const { return reinterpret_cast<const sockaddr*>(&m_address); }
	socklen_t Size() const { return sizeof(m_address); }

private:
	sockaddr_un m_address{};
	int m_directory = -1;
};
} // namespace simxr::control
