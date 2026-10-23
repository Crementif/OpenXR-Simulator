#include "control/LocalControlClient.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "platform/linux/ControlFd.h"

#include <cerrno>
#include <climits>
#include <cstring>
#include <mutex>
#include <poll.h>
#include <sys/eventfd.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <system_error>
#include <unistd.h>

namespace simxr::control
{
using Clock = std::chrono::steady_clock;
static void Ready(int socket, int stopping, short events, Clock::time_point deadline, RequestId request)
{
	for (;;)
	{
		const auto remaining = deadline - Clock::now();
		if (remaining <= Clock::duration::zero())
			throw ControlExchangeTimeout{request};
		const auto milliseconds = std::chrono::ceil<std::chrono::milliseconds>(remaining).count();
		std::array<pollfd, 2> items{{{socket, events, 0}, {stopping, POLLIN, 0}}};
		const auto result =
			::poll(items.data(), items.size(), static_cast<int>(std::min<std::int64_t>(milliseconds, INT_MAX)));
		if (result < 0 && errno == EINTR)
			continue;
		if (result < 0)
			throw std::system_error(errno, std::generic_category(), "Control socket polling failed");
		if (items[1].revents)
			throw std::runtime_error("Control connection is closed");
		if (items[0].revents & (POLLERR | POLLNVAL))
			throw std::runtime_error("Control socket polling failed");
		if (!result)
			throw ControlExchangeTimeout{request};
		if (items[0].revents & events)
			return;
		if (items[0].revents & POLLHUP)
			throw std::runtime_error("Control peer disconnected");
	}
}
ControlExchangeTimeout::ControlExchangeTimeout(RequestId request)
	: std::runtime_error("Control exchange is unfinished; Retry or Close explicitly"), m_request(request)
{
}
RequestId ControlExchangeTimeout::Request() const noexcept
{
	return m_request;
}

struct LocalControlClient::State
{
	struct Descriptor
	{
		int value = -1;
		~Descriptor()
		{
			if (value >= 0)
				::close(value);
		}
	};
	struct Rights
	{
		std::array<int, MaxTransferHandles> values{};
		std::size_t count = 0;
		~Rights()
		{
			for (std::size_t index = 0; index < count; ++index)
				if (values[index] >= 0)
					::close(values[index]);
		}
	};
	struct Exchange
	{
		ControlPacket outgoing, incoming;
		std::vector<std::byte> encoded, received;
		std::size_t sent = 0, offset = 0;
		bool handlesSent = false;
	};
	mutable std::mutex mutex;
	Descriptor socket;
	Descriptor stopping;
	std::chrono::milliseconds timeout;
	std::uint64_t next = 1;
	std::unique_ptr<Exchange> pending;
	State(std::string path, std::chrono::milliseconds timeout) : timeout(timeout)
	{
		if (timeout.count() <= 0 || timeout > std::chrono::hours{24} || path.empty() || path.front() != '/' ||
			path.find('\0') != std::string::npos || path.size() >= sizeof(sockaddr_un::sun_path))
			throw std::invalid_argument("Invalid local control path or timeout");
		struct stat info{};
		if (::lstat(path.c_str(), &info) != 0 || !S_ISSOCK(info.st_mode) || info.st_uid != ::geteuid() ||
			info.st_mode & 0077)
			throw std::runtime_error("Control endpoint must be a private socket owned by this user");
		stopping.value = ::eventfd(0, EFD_CLOEXEC | EFD_NONBLOCK);
		if (stopping.value < 0)
			throw std::system_error(errno, std::generic_category(), "Create control stop event failed");
		socket.value = ::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC | SOCK_NONBLOCK, 0);
		if (socket.value < 0)
			throw std::runtime_error("Create local control socket failed");
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::memcpy(address.sun_path, path.c_str(), path.size() + 1);
		if (::connect(socket.value, reinterpret_cast<sockaddr*>(&address), sizeof(address)) != 0)
		{
			if (errno != EINPROGRESS && errno != EAGAIN)
				throw std::runtime_error("Connect local control socket failed");
			Ready(socket.value, stopping.value, POLLOUT, Clock::now() + timeout, {});
			int error = 0;
			socklen_t size = sizeof(error);
			if (::getsockopt(socket.value, SOL_SOCKET, SO_ERROR, &error, &size) != 0 || error)
				throw std::runtime_error("Connect local control socket failed");
		}
		ucred peer{};
		socklen_t size = sizeof(peer);
		if (::getsockopt(socket.value, SOL_SOCKET, SO_PEERCRED, &peer, &size) != 0 || peer.uid != ::geteuid())
			throw std::runtime_error("Control peer belongs to another user");
	}
	std::unique_ptr<Exchange> Close()
	{
		if (socket.value >= 0)
			::close(std::exchange(socket.value, -1));
		return std::move(pending);
	}
	void Send(Clock::time_point deadline)
	{
		auto& exchange = *pending;
		while (exchange.sent < exchange.encoded.size())
		{
			Ready(socket.value, stopping.value, POLLOUT, deadline, exchange.outgoing.message.requestId);
			ssize_t sent;
			if (!exchange.handlesSent && !exchange.outgoing.handles.empty())
			{
				std::array<int, MaxTransferHandles> fds{};
				for (std::size_t index = 0; index < exchange.outgoing.handles.size(); ++index)
					fds[index] = static_cast<int>(exchange.outgoing.handles[index]->Value());
				alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int) * MaxTransferHandles)> control{};
				iovec data{exchange.encoded.data() + exchange.sent, exchange.encoded.size() - exchange.sent};
				msghdr message{};
				message.msg_iov = &data;
				message.msg_iovlen = 1;
				message.msg_control = control.data();
				message.msg_controllen = CMSG_SPACE(sizeof(int) * exchange.outgoing.handles.size());
				auto* rights = CMSG_FIRSTHDR(&message);
				rights->cmsg_level = SOL_SOCKET;
				rights->cmsg_type = SCM_RIGHTS;
				rights->cmsg_len = CMSG_LEN(sizeof(int) * exchange.outgoing.handles.size());
				std::memcpy(CMSG_DATA(rights), fds.data(), sizeof(int) * exchange.outgoing.handles.size());
				sent = ::sendmsg(socket.value, &message, MSG_NOSIGNAL);
				if (sent > 0)
				{
					if (exchange.outgoing.exposure)
						exchange.outgoing.exposure->MarkTransferred();
					exchange.handlesSent = true;
					exchange.outgoing.handles.clear();
				}
			}
			else
				sent = ::send(socket.value, exchange.encoded.data() + exchange.sent,
							  exchange.encoded.size() - exchange.sent, MSG_NOSIGNAL);
			if (sent < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
				continue;
			if (sent <= 0)
				throw std::system_error(errno, std::generic_category(), "Control request send failed");
			exchange.sent += static_cast<std::size_t>(sent);
		}
	}
	void Receive(Clock::time_point deadline)
	{
		auto& exchange = *pending;
		while (exchange.offset < exchange.received.size())
		{
			Ready(socket.value, stopping.value, POLLIN, deadline, exchange.outgoing.message.requestId);
			alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int) * MaxTransferHandles)> control{};
			iovec data{exchange.received.data() + exchange.offset, exchange.received.size() - exchange.offset};
			msghdr message{};
			message.msg_iov = &data;
			message.msg_iovlen = 1;
			message.msg_control = control.data();
			message.msg_controllen = control.size();
			const auto count = ::recvmsg(socket.value, &message, MSG_CMSG_CLOEXEC);
			if (count < 0 && (errno == EINTR || errno == EAGAIN || errno == EWOULDBLOCK))
				continue;
			Rights rights;
			bool valid = !(message.msg_flags & ~MSG_CMSG_CLOEXEC);
			if (count >= 0)
				for (auto* item = CMSG_FIRSTHDR(&message); item; item = CMSG_NXTHDR(&message, item))
				{
					const auto offset = reinterpret_cast<const std::byte*>(item) - control.data();
					if (item->cmsg_len < CMSG_LEN(0) || item->cmsg_len > message.msg_controllen - offset)
					{
						valid = false;
						break;
					}
					if (item->cmsg_level != SOL_SOCKET || item->cmsg_type != SCM_RIGHTS)
					{
						valid = false;
						continue;
					}
					const auto bytes = item->cmsg_len - CMSG_LEN(0);
					if (bytes % sizeof(int))
						valid = false;
					for (std::size_t index = 0; index + sizeof(int) <= bytes; index += sizeof(int))
					{
						int fd;
						std::memcpy(&fd, reinterpret_cast<const std::byte*>(CMSG_DATA(item)) + index, sizeof(fd));
						if (fd < 0)
						{
							valid = false;
							continue;
						}
						if (rights.count == rights.values.size())
						{
							::close(fd);
							valid = false;
						}
						else
							rights.values[rights.count++] = fd;
					}
				}
			if (count <= 0 || !valid || rights.count + exchange.incoming.handles.size() > MaxTransferHandles)
				throw std::runtime_error("Invalid control response ancillary data or disconnected peer");
			for (std::size_t index = 0; index < rights.count; ++index)
				exchange.incoming.handles.push_back(OwnControlFd(std::exchange(rights.values[index], -1)));
			exchange.offset += static_cast<std::size_t>(count);
		}
	}
	ControlPacket ExchangePacket()
	{
		const auto deadline = Clock::now() + timeout;
		Send(deadline);
		for (;;)
		{
			Receive(deadline);
			auto decoded = Decode(pending->received);
			if (decoded.error == DecodeError::Incomplete && decoded.requiredBytes > pending->received.size())
			{
				pending->received.resize(decoded.requiredBytes);
				continue;
			}
			if (!decoded.message)
				throw std::runtime_error("Invalid control response framing");
			auto& response = *decoded.message;
			if (response.flags != 1 || response.requestId != pending->outgoing.message.requestId ||
				response.opcode != pending->outgoing.message.opcode)
				throw std::runtime_error("Control response identity mismatch");
			const auto* count = response.Find(tag::HandleCount);
			const auto declared = count ? count->AsU32() : std::optional<std::uint32_t>{0};
			const auto* error = response.Find(tag::Error);
			if (!declared || !error || !error->AsU32() || *declared != pending->incoming.handles.size() ||
				(*error->AsU32() && !pending->incoming.handles.empty()) ||
				(!pending->incoming.handles.empty() &&
				 response.opcode != static_cast<std::uint16_t>(OpCode::ShareImage)) ||
				(!*error->AsU32() && response.opcode == static_cast<std::uint16_t>(OpCode::ShareImage) &&
				 *declared != 2))
				throw std::runtime_error("Control response handle count mismatch");
			pending->incoming.message = std::move(response);
			return std::move(pending->incoming);
		}
	}
};
LocalControlClient::LocalControlClient(std::string path, std::chrono::milliseconds timeout)
	: m_state(std::make_unique<State>(std::move(path), timeout))
{
}
LocalControlClient::~LocalControlClient()
{
	Close();
}
ControlPacket LocalControlClient::Request(ControlPacket request)
{
	std::unique_ptr<State::Exchange> retired;
	std::unique_ptr<State::Exchange> exchange;
	std::unique_lock lock(m_state->mutex);
	if (m_state->socket.value < 0)
		throw std::runtime_error("Control connection is closed");
	if (m_state->pending)
		throw std::logic_error("Control exchange unfinished; Retry or Close explicitly");
	exchange = std::make_unique<State::Exchange>();
	if (request.message.flags)
		throw std::invalid_argument("Expected a control request");
	if (request.handles.size() > MaxTransferHandles)
		throw std::invalid_argument("Too many request handles");
	for (const auto& handle : request.handles)
	{
		if (!handle || handle->Kind() != TransferHandleKind::OpaqueFd || handle->Value() < 0 ||
			handle->Value() > INT_MAX)
			throw std::invalid_argument("Invalid request handle");
		exchange->outgoing.handles.push_back(handle->Duplicate());
	}
	if (const auto* count = request.message.Find(tag::HandleCount))
	{
		if (count->AsU32() != request.handles.size())
			throw std::invalid_argument("Request handle count mismatch");
	}
	else if (!request.handles.empty())
		request.message.fields.push_back(
			Field::U32(tag::HandleCount, static_cast<std::uint32_t>(request.handles.size()), true));
	if (!request.message.requestId.value)
	{
		if (!m_state->next)
			throw std::overflow_error("Control request identity exhausted");
		request.message.requestId = {m_state->next};
	}
	exchange->outgoing.message = std::move(request.message);
	exchange->outgoing.exposure = std::move(request.exposure);
	exchange->encoded = Encode(exchange->outgoing.message);
	exchange->received.resize(HeaderSize);
	if (m_state->next && exchange->outgoing.message.requestId.value >= m_state->next)
		m_state->next = exchange->outgoing.message.requestId.value == UINT64_MAX
							? 0
							: exchange->outgoing.message.requestId.value + 1;
	m_state->pending = std::move(exchange);
	try
	{
		auto result = m_state->ExchangePacket();
		retired = std::move(m_state->pending);
		return result;
	}
	catch (const ControlExchangeTimeout&)
	{
		throw;
	}
	catch (...)
	{
		retired = m_state->Close();
		throw;
	}
}
ControlPacket LocalControlClient::Retry()
{
	std::unique_ptr<State::Exchange> retired;
	std::unique_lock lock(m_state->mutex);
	if (!m_state->pending)
		throw std::logic_error("No unfinished control exchange");
	try
	{
		auto result = m_state->ExchangePacket();
		retired = std::move(m_state->pending);
		return result;
	}
	catch (const ControlExchangeTimeout&)
	{
		throw;
	}
	catch (...)
	{
		retired = m_state->Close();
		throw;
	}
}
std::optional<RequestId> LocalControlClient::Pending() const
{
	std::lock_guard lock(m_state->mutex);
	return m_state->pending ? std::optional{m_state->pending->outgoing.message.requestId} : std::nullopt;
}
void LocalControlClient::Close() noexcept
{
	while (::eventfd_write(m_state->stopping.value, 1) < 0 && errno == EINTR)
	{
	}
	std::unique_ptr<State::Exchange> retired;
	std::lock_guard lock(m_state->mutex);
	retired = m_state->Close();
}
} // namespace simxr::control
