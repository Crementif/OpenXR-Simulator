#include "control/LocalControlServer.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "platform/linux/ControlFd.h"

#include <atomic>
#include <cerrno>
#include <climits>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <stdexcept>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>
#include <unistd.h>

namespace simxr::control
{
using Clock = std::chrono::steady_clock;
static constexpr auto s_packetTimeout = std::chrono::seconds(5);

static bool Ready(int socket, short events, const std::atomic<bool>& stopping, Clock::time_point deadline)
{
	while (!stopping.load())
	{
		if (Clock::now() >= deadline)
			return false;
		pollfd descriptor{socket, events, 0};
		const auto result = poll(&descriptor, 1, 100);
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0 || descriptor.revents & (POLLERR | POLLNVAL))
			return false;
		if (descriptor.revents & events)
			return true;
		if (descriptor.revents & POLLHUP)
			return false;
	}
	return false;
}

static bool Read(int socket, std::vector<std::byte>& bytes, std::size_t total, ControlPacket& packet,
				 const std::atomic<bool>& stopping, Clock::time_point& deadline)
{
	struct ReceivedRights
	{
		std::array<int, MaxTransferHandles> values{};
		std::size_t count = 0;
		~ReceivedRights()
		{
			for (std::size_t index = 0; index < count; ++index)
				if (values[index] >= 0)
					close(values[index]);
		}
	};
	auto offset = bytes.size();
	bytes.resize(total);
	while (offset < total)
	{
		if (!Ready(socket, POLLIN, stopping, deadline))
		{
			bytes.resize(offset);
			return false;
		}
		alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int) * MaxTransferHandles)> control{};
		iovec data{bytes.data() + offset, total - offset};
		msghdr message{};
		message.msg_iov = &data;
		message.msg_iovlen = 1;
		message.msg_control = control.data();
		message.msg_controllen = control.size();
		const auto received = recvmsg(socket, &message, MSG_CMSG_CLOEXEC);
		if (received < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		ReceivedRights rights;
		bool valid = !(message.msg_flags & ~MSG_CMSG_CLOEXEC);
		// First collect every raw right without allocation. An allocation while
		// adopting one handle must never leak descriptors later in this batch.
		if (received >= 0)
			for (auto* ancillary = CMSG_FIRSTHDR(&message); ancillary; ancillary = CMSG_NXTHDR(&message, ancillary))
			{
				const auto position = reinterpret_cast<const std::byte*>(ancillary) - control.data();
				if (ancillary->cmsg_len < CMSG_LEN(0) || ancillary->cmsg_len > message.msg_controllen - position)
				{
					valid = false;
					break;
				}
				if (ancillary->cmsg_level != SOL_SOCKET || ancillary->cmsg_type != SCM_RIGHTS)
				{
					valid = false;
					continue;
				}
				const auto size = ancillary->cmsg_len - CMSG_LEN(0);
				if (size % sizeof(int))
					valid = false;
				for (std::size_t index = 0; index + sizeof(int) <= size; index += sizeof(int))
				{
					int fd;
					std::memcpy(&fd, reinterpret_cast<const std::byte*>(CMSG_DATA(ancillary)) + index, sizeof(fd));
					if (fd < 0)
					{
						valid = false;
						continue;
					}
					if (rights.count == rights.values.size())
					{
						close(fd);
						valid = false;
						continue;
					}
					rights.values[rights.count++] = fd;
				}
			}
		if (received <= 0 || !valid || packet.handles.size() + rights.count > MaxTransferHandles)
		{
			bytes.resize(offset);
			return false;
		}
		for (std::size_t index = 0; index < rights.count; ++index)
			packet.handles.push_back(OwnControlFd(std::exchange(rights.values[index], -1)));
		offset += static_cast<std::size_t>(received);
		if (deadline == Clock::time_point::max())
			deadline = Clock::now() + s_packetTimeout;
	}
	return true;
}
static bool Write(int socket, const ControlPacket& packet, const std::atomic<bool>& stopping)
{
	std::array<int, MaxTransferHandles> descriptors{};
	if (packet.handles.size() > descriptors.size())
		return false;
	for (std::size_t index = 0; index < packet.handles.size(); ++index)
	{
		const auto& handle = packet.handles[index];
		if (!handle || handle->Kind() != TransferHandleKind::OpaqueFd || handle->Value() < 0 ||
			handle->Value() > INT_MAX)
			return false;
		descriptors[index] = static_cast<int>(handle->Value());
	}
	const auto fallback = packet.encoded ? std::vector<std::byte>{} : Encode(packet.message);
	const std::span<const std::byte> bytes = packet.encoded ? std::span<const std::byte>{*packet.encoded} : fallback;
	const auto deadline = Clock::now() + s_packetTimeout;
	std::size_t offset = 0;
	bool transferred = false;
	while (offset < bytes.size())
	{
		if (!Ready(socket, POLLOUT, stopping, deadline))
			return false;
		ssize_t sent;
		if (!transferred && !packet.handles.empty())
		{
			alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int) * MaxTransferHandles)> control{};
			iovec data{const_cast<std::byte*>(bytes.data() + offset), bytes.size() - offset};
			msghdr message{};
			message.msg_iov = &data;
			message.msg_iovlen = 1;
			message.msg_control = control.data();
			message.msg_controllen = CMSG_SPACE(sizeof(int) * packet.handles.size());
			auto* ancillary = CMSG_FIRSTHDR(&message);
			ancillary->cmsg_level = SOL_SOCKET;
			ancillary->cmsg_type = SCM_RIGHTS;
			ancillary->cmsg_len = CMSG_LEN(sizeof(int) * packet.handles.size());
			std::memcpy(CMSG_DATA(ancillary), descriptors.data(), sizeof(int) * packet.handles.size());
			sent = sendmsg(socket, &message, MSG_NOSIGNAL);
			if (sent > 0)
			{
				if (packet.exposure)
					packet.exposure->MarkTransferred();
				transferred = true;
			}
		}
		else
			sent = send(socket, bytes.data() + offset, bytes.size() - offset, MSG_NOSIGNAL);
		if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
			continue;
		if (sent <= 0)
			return false;
		offset += static_cast<std::size_t>(sent);
	}
	return true;
}

static std::uint64_t ReadHeaderInteger(std::span<const std::byte> bytes, std::size_t offset, std::size_t size)
{
	std::uint64_t value = 0;
	for (std::size_t index = 0; index < size; ++index)
		value |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + index])} << (index * 8);
	return value;
}
struct LocalControlServer::State
{
	struct Descriptor
	{
		int value = -1;
		~Descriptor()
		{
			if (value >= 0)
				close(value);
		}
	};
	struct Worker
	{
		int socket = -1;
		std::atomic<bool> finished{false};
		std::thread thread;
	};
	ControlService& service;
	std::string path;
	const std::size_t capacity;
	Descriptor listener;
	struct stat bound{};
	bool ownsPath = false;
	std::atomic<bool> stopping{false};
	std::mutex lifecycle;
	std::thread acceptor;
	std::vector<std::unique_ptr<Worker>> workers;
	State(ControlService& service, std::string path, std::size_t capacity)
		: service(service), path(std::move(path)), capacity(capacity)
	{
		if (!capacity || capacity > 64 || this->path.empty() || this->path.front() != '/' ||
			this->path.find('\0') != std::string::npos || this->path.size() >= sizeof(sockaddr_un::sun_path))
			throw std::invalid_argument("Invalid local control socket path/capacity");
		workers.reserve(capacity);
	}
	~State()
	{
		struct stat current{};
		if (ownsPath && lstat(path.c_str(), &current) == 0 && current.st_dev == bound.st_dev &&
			current.st_ino == bound.st_ino)
			unlink(path.c_str());
	}
	void Bind()
	{
		listener.value = socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (listener.value < 0)
			throw std::runtime_error("Create local control socket failed");
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
		struct stat existing{};
		if (lstat(path.c_str(), &existing) == 0)
		{
			if (!S_ISSOCK(existing.st_mode) || existing.st_uid != geteuid())
				throw std::runtime_error("Local control path already exists and is not an owned socket");
			Descriptor probe{socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0)};
			if (probe.value < 0 || connect(probe.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 ||
				errno != ECONNREFUSED)
				throw std::runtime_error("Local control socket already active");
			struct stat current{};
			if (lstat(path.c_str(), &current) != 0 || current.st_dev != existing.st_dev ||
				current.st_ino != existing.st_ino || unlink(path.c_str()) != 0)
				throw std::runtime_error("Cannot retire stale local control socket");
		}
		if (bind(listener.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
			throw std::runtime_error("Bind local control socket failed");
		if (lstat(path.c_str(), &bound) != 0)
			throw std::runtime_error("Inspect bound local control socket failed");
		ownsPath = true;
		if (chmod(path.c_str(), 0600) != 0 || listen(listener.value, static_cast<int>(capacity)) != 0)
			throw std::runtime_error("Protect/listen local control socket failed");
	}
	void Serve(Worker& worker, ConnectionId connection) noexcept
	{
		const auto trace = [&](const char* stage, std::uint16_t opcode = 0, std::uint64_t request = 0)
		{
			if (std::getenv("SIMXR_CONTROL_TRACE"))
				std::fprintf(stderr, "SimXR control trace connection=%llu stage=%s opcode=%u request=%llu\n",
							 static_cast<unsigned long long>(connection.value), stage, opcode,
							 static_cast<unsigned long long>(request));
		};
		try
		{
			while (!stopping.load())
			{
				std::vector<std::byte> bytes;
				ControlPacket packet;
				auto deadline = Clock::time_point::max();
				if (!Read(worker.socket, bytes, HeaderSize, packet, stopping, deadline))
				{
					trace("header-read-failed");
					break;
				}
				auto decoded = Decode(bytes);
				if (decoded.error == DecodeError::UnsupportedVersion)
				{
					// A valid identifiable envelope can receive a structured version error.
					if (ReadHeaderInteger(bytes, 0, 4) != Magic || ReadHeaderInteger(bytes, 10, 2) ||
						ReadHeaderInteger(bytes, 12, 4) || ReadHeaderInteger(bytes, 28, 4) ||
						!ReadHeaderInteger(bytes, 16, 8))
						break;
					Message response;
					response.flags = 1;
					response.opcode = static_cast<std::uint16_t>(ReadHeaderInteger(bytes, 8, 2));
					response.requestId = {ReadHeaderInteger(bytes, 16, 8)};
					response.fields.push_back(
						Field::U32(tag::Error, static_cast<std::uint32_t>(ErrorCode::UnsupportedVersion), true));
					Write(worker.socket, ControlPacket{std::move(response), {}, {}, {}}, stopping);
					break;
				}
				if (decoded.error == DecodeError::Incomplete)
				{
					if (!Read(worker.socket, bytes, decoded.requiredBytes, packet, stopping, deadline))
					{
						trace("payload-read-failed");
						break;
					}
					decoded = Decode(bytes);
				}
				if (!decoded.message || decoded.message->flags)
				{
					trace("invalid-request");
					break;
				}
				packet.message = std::move(*decoded.message);
				trace("request", packet.message.opcode, packet.message.requestId.value);
				const auto response = service.HandlePacket(connection, std::move(packet));
				if (!Write(worker.socket, response, stopping))
				{
					trace("response-write-failed", response.message.opcode, response.message.requestId.value);
					break;
				}
			}
		}
		catch (const std::exception& error)
		{
			std::fprintf(stderr, "SimXR control: connection failed: %s\n", error.what());
		}
		catch (...)
		{
			std::fputs("SimXR control: connection failed\n", stderr);
		}
		service.Disconnect(connection);
		// Close owns the descriptor after joining; its integer cannot be recycled
		// while the acceptor or shutdown path still refers to this worker.
		shutdown(worker.socket, SHUT_RDWR);
		worker.finished.store(true);
	}
	void Collect()
	{
		for (auto& worker : workers)
			if (worker->finished.load())
			{
				worker->thread.join();
				close(worker->socket);
				worker.reset();
			}
		std::erase(workers, nullptr);
	}
	void Accept() noexcept
	{
		while (!stopping.load())
		{
			Collect();
			if (!Ready(listener.value, POLLIN, stopping, Clock::time_point::max()))
				break;
			Collect();
			Descriptor accepted{accept4(listener.value, nullptr, nullptr, SOCK_CLOEXEC | SOCK_NONBLOCK)};
			if (accepted.value < 0)
				continue;
			ucred peer{};
			socklen_t size = sizeof(peer);
			if (getsockopt(accepted.value, SOL_SOCKET, SO_PEERCRED, &peer, &size) || peer.uid != geteuid() ||
				workers.size() >= capacity)
				continue;
			std::optional<ConnectionId> connection;
			try
			{
				connection = service.Connect(true);
				if (!connection)
					continue;
				auto worker = std::make_unique<Worker>();
				worker->socket = accepted.value;
				auto* raw = worker.get();
				workers.push_back(std::move(worker));
				try
				{
					raw->thread = std::thread(
						[this, raw, id = *connection]
						{
							Serve(*raw, id);
						});
				}
				catch (...)
				{
					workers.pop_back();
					throw;
				}
				accepted.value = -1;
			}
			catch (...)
			{
				if (connection)
					service.Disconnect(*connection);
				std::fputs("SimXR control: connection capacity/allocation failure\n", stderr);
			}
		}
	}
};
LocalControlServer::LocalControlServer(ControlService& service, std::string path, std::size_t maxClients)
	: m_state(std::make_unique<State>(service, std::move(path), maxClients))
{
	m_state->Bind();
	m_state->acceptor = std::thread(
		[state = m_state.get()]
		{
			state->Accept();
		});
}
LocalControlServer::~LocalControlServer()
{
	Close();
}
const std::string& LocalControlServer::Path() const
{
	return m_state->path;
}
void LocalControlServer::Close() noexcept
{
	std::lock_guard lock(m_state->lifecycle);
	if (m_state->stopping.exchange(true))
		return;
	shutdown(m_state->listener.value, SHUT_RDWR);
	if (m_state->acceptor.joinable())
		m_state->acceptor.join();
	if (m_state->listener.value >= 0)
		close(std::exchange(m_state->listener.value, -1));
	for (auto& worker : m_state->workers)
		shutdown(worker->socket, SHUT_RDWR);
	for (auto& worker : m_state->workers)
	{
		if (worker->thread.joinable())
			worker->thread.join();
		close(worker->socket);
	}
	m_state->workers.clear();
	struct stat current{};
	if (m_state->ownsPath && lstat(m_state->path.c_str(), &current) == 0 && current.st_dev == m_state->bound.st_dev &&
		current.st_ino == m_state->bound.st_ino)
		unlink(m_state->path.c_str());
	m_state->ownsPath = false;
}
} // namespace simxr::control
