#include "app/mcp/McpPch.h"
#include "app/mcp/windows/McpCancelablePost.h"

#include <winsock2.h>
#include <ws2tcpip.h>
#include <cctype>

namespace simxr::app::windows
{
using namespace std::chrono_literals;
static constexpr std::size_t MaxBody = 16 * 1024 * 1024;
static constexpr std::size_t MaxHeaders = 64 * 1024;
static constexpr std::size_t MaxWire = MaxBody + 1024 * 1024;

class Socket
{
  public:
	Socket()
	{
		WSADATA data{};
		if (WSAStartup(MAKEWORD(2, 2), &data) != 0)
			return;
		m_started = true;
		m_socket = socket(AF_INET, SOCK_STREAM, IPPROTO_TCP);
		if (m_socket != INVALID_SOCKET)
		{
			u_long nonblocking = 1;
			if (ioctlsocket(m_socket, FIONBIO, &nonblocking) != 0)
			{
				closesocket(m_socket);
				m_socket = INVALID_SOCKET;
			}
		}
	}
	~Socket()
	{
		if (m_socket != INVALID_SOCKET)
			closesocket(m_socket);
		if (m_started)
			WSACleanup();
	}
	Socket(const Socket&) = delete;
	Socket& operator=(const Socket&) = delete;
	SOCKET Get() const noexcept { return m_socket; }

  private:
	SOCKET m_socket = INVALID_SOCKET;
	bool m_started = false;
};
static bool Ready(SOCKET socket, short events, const std::atomic<bool>& stopped,
				  std::chrono::steady_clock::time_point deadline)
{
	while (!stopped.load() && std::chrono::steady_clock::now() < deadline)
	{
		WSAPOLLFD descriptor{socket, events, 0};
		const auto count = WSAPoll(&descriptor, 1, 50);
		if (count > 0)
			return (descriptor.revents & (events | POLLHUP)) != 0 && !(descriptor.revents & (POLLERR | POLLNVAL));
		if (count < 0 && WSAGetLastError() != WSAEINTR)
			return false;
	}
	return false;
}
static std::string Lower(std::string value)
{
	for (auto& byte : value)
		byte = static_cast<char>(std::tolower(static_cast<unsigned char>(byte)));
	return value;
}
static std::string_view Trim(std::string_view value)
{
	while (!value.empty() && (value.front() == ' ' || value.front() == '\t'))
		value.remove_prefix(1);
	while (!value.empty() && (value.back() == ' ' || value.back() == '\t'))
		value.remove_suffix(1);
	return value;
}
struct ResponseState
{
	int status = 0;
	std::optional<std::size_t> length;
	bool chunked = false, parsed = false, complete = false;
	std::size_t start = 0, cursor = 0;
	std::string body;
};
static bool ParseHeaders(std::string_view wire, ResponseState& result)
{
	const auto end = wire.find("\r\n\r\n");
	if (end == std::string_view::npos)
		return wire.size() <= MaxHeaders;
	if (end > MaxHeaders)
		return false;
	const auto firstEnd = wire.find("\r\n");
	if (firstEnd == std::string_view::npos || firstEnd > end)
		return false;
	const auto statusLine = wire.substr(0, firstEnd);
	if (!statusLine.starts_with("HTTP/1.0 ") && !statusLine.starts_with("HTTP/1.1 "))
		return false;
	if (statusLine.size() < 12 || (statusLine.size() > 12 && statusLine[12] != ' '))
		return false;
	const auto code = statusLine.substr(9, 3);
	const auto parsed = std::from_chars(code.data(), code.data() + code.size(), result.status);
	if (parsed.ec != std::errc{} || parsed.ptr != code.data() + code.size() || result.status < 100 || result.status > 599)
		return false;
	for (auto offset = firstEnd + 2; offset < end;)
	{
		const auto next = wire.find("\r\n", offset);
		if (next == std::string_view::npos || next > end)
			return false;
		const auto line = wire.substr(offset, next - offset);
		const auto colon = line.find(':');
		if (!colon || colon == std::string_view::npos)
			return false;
		const auto name = Lower(std::string(line.substr(0, colon)));
		const auto value = Trim(line.substr(colon + 1));
		if (name == "content-length")
		{
			std::uint64_t length = 0;
			const auto number = std::from_chars(value.data(), value.data() + value.size(), length);
			if (value.empty() || number.ec != std::errc{} || number.ptr != value.data() + value.size() || length > MaxBody || result.length)
				return false;
			result.length = static_cast<std::size_t>(length);
		}
		else if (name == "transfer-encoding")
		{
			if (result.chunked || Lower(std::string(value)) != "chunked")
				return false;
			result.chunked = true;
		}
		offset = next + 2;
	}
	if (result.chunked && result.length)
		return false;
	result.parsed = true;
	result.start = end + 4;
	result.cursor = result.start;
	return true;
}
static bool ParseBody(std::string_view wire, ResponseState& result, bool eof)
{
	if (!result.parsed)
		return !eof;
	if (result.length)
	{
		if (wire.size() - result.start >= *result.length)
		{
			result.body.assign(wire.substr(result.start, *result.length));
			result.complete = true;
		}
		return result.complete || !eof;
	}
	if (!result.chunked)
	{
		if (wire.size() - result.start > MaxBody)
			return false;
		if (eof)
		{
			result.body.assign(wire.substr(result.start));
			result.complete = true;
		}
		return true;
	}
	while (result.cursor < wire.size())
	{
		const auto lineEnd = wire.find("\r\n", result.cursor);
		if (lineEnd == std::string_view::npos)
			return !eof && wire.size() - result.cursor <= MaxHeaders;
		const auto line = wire.substr(result.cursor, lineEnd - result.cursor);
		const auto extension = line.find(';');
		const auto number = line.substr(0, extension);
		std::uint64_t length = 0;
		const auto parsed = std::from_chars(number.data(), number.data() + number.size(), length, 16);
		if (number.empty() || parsed.ec != std::errc{} || parsed.ptr != number.data() + number.size() || length > MaxBody - result.body.size())
			return false;
		const auto content = lineEnd + 2;
		if (!length)
		{
			if (wire.size() >= content + 2 && wire.substr(content, 2) == "\r\n")
			{
				result.complete = true;
				return true;
			}
			const auto trailers = wire.find("\r\n\r\n", content);
			if (trailers != std::string_view::npos && trailers - content <= MaxHeaders)
			{
				result.complete = true;
				return true;
			}
			return !eof && wire.size() - content <= MaxHeaders;
		}
		if (length > wire.size() - content || wire.size() - content - static_cast<std::size_t>(length) < 2)
			return !eof;
		const auto end = content + static_cast<std::size_t>(length);
		if (wire.substr(end, 2) != "\r\n")
			return false;
		result.body.append(wire.substr(content, static_cast<std::size_t>(length)));
		result.cursor = end + 2;
	}
	return !eof;
}
httplib::Result CancelablePost(std::uint16_t port, const httplib::Headers& headers, const std::string& body,
								  const std::atomic<bool>& stopped, std::chrono::seconds timeout)
{
	const auto failed = [] { return httplib::Result(nullptr, httplib::Error::Read); };
	if (stopped.load() || body.size() > 1024 * 1024)
		return failed();
	Socket socket;
	if (socket.Get() == INVALID_SOCKET)
		return failed();
	sockaddr_in address{};
	address.sin_family = AF_INET;
	address.sin_port = htons(port);
	address.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
	if (connect(socket.Get(), reinterpret_cast<const sockaddr*>(&address), sizeof(address)) == SOCKET_ERROR)
	{
		const auto error = WSAGetLastError();
		if (error != WSAEWOULDBLOCK && error != WSAEINPROGRESS && error != WSAEINVAL)
			return failed();
		if (!Ready(socket.Get(), POLLWRNORM, stopped, std::chrono::steady_clock::now() + 1s))
			return failed();
		int connected = 0, size = sizeof(connected);
		if (getsockopt(socket.Get(), SOL_SOCKET, SO_ERROR, reinterpret_cast<char*>(&connected), &size) != 0 || connected)
			return failed();
	}
	std::string request = "POST /mcp HTTP/1.1\r\nHost: 127.0.0.1:" + std::to_string(port) +
		"\r\nConnection: close\r\nContent-Type: application/json\r\nContent-Length: " + std::to_string(body.size()) + "\r\n";
	for (const auto& [name, value] : headers)
	{
		if (name.empty() || name.find_first_of("\r\n:") != std::string::npos || value.find_first_of("\r\n") != std::string::npos)
			return failed();
		request += name + ": " + value + "\r\n";
	}
	request += "\r\n";
	request += body;
	const auto writeDeadline = std::chrono::steady_clock::now() + 2s;
	std::size_t sent = 0;
	while (sent < request.size())
	{
		if (!Ready(socket.Get(), POLLWRNORM, stopped, writeDeadline))
			return failed();
		const auto count = send(socket.Get(), request.data() + sent, static_cast<int>(request.size() - sent), 0);
		if (count == SOCKET_ERROR)
		{
			if (WSAGetLastError() == WSAEWOULDBLOCK)
				continue;
			return failed();
		}
		if (!count)
			return failed();
		sent += static_cast<std::size_t>(count);
	}
	const auto deadline = std::chrono::steady_clock::now() + timeout;
	std::string wire;
	ResponseState state;
	std::array<char, 16384> buffer{};
	while (!state.complete)
	{
		if (!Ready(socket.Get(), POLLRDNORM, stopped, deadline))
			return failed();
		const auto count = recv(socket.Get(), buffer.data(), static_cast<int>(buffer.size()), 0);
		if (count == SOCKET_ERROR)
		{
			if (WSAGetLastError() == WSAEWOULDBLOCK)
				continue;
			return failed();
		}
		const bool eof = count == 0;
		if (!eof)
		{
			if (static_cast<std::size_t>(count) > MaxWire - wire.size())
				return failed();
			wire.append(buffer.data(), static_cast<std::size_t>(count));
		}
		if ((!state.parsed && !ParseHeaders(wire, state)) || !ParseBody(wire, state, eof))
			return failed();
	}
	auto response = std::make_unique<httplib::Response>();
	response->status = state.status;
	response->body = std::move(state.body);
	return {std::move(response), httplib::Error::Success};
}
} // namespace simxr::app::windows
