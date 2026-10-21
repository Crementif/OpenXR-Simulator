#include "control/LocalControlClient.h"
#include "control/ControlSchema.h"
#include "platform/windows/WindowsControl.h"

#include <limits>
#include <mutex>

namespace simxr::control
{
namespace native = windows;
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
	struct Exchange
	{
		Message outgoing;
		std::vector<std::byte> encoded, received;
		std::size_t sent = 0, offset = 0;
	};
	mutable std::mutex mutex;
	native::Handle stopping = native::Event();
	std::unique_ptr<native::Channel> pipe;
	std::unique_ptr<Exchange> pending;
	std::chrono::milliseconds timeout;
	std::uint64_t next = 1;
	State(const std::string& path, std::chrono::milliseconds timeout) : timeout(timeout)
	{
		if (timeout.count() <= 0 || timeout > std::chrono::hours{24})
			throw std::invalid_argument("Invalid control pipe timeout");
		const auto wide = native::PipePath(path);
		const auto deadline = native::Clock::now() + timeout;
		native::Handle connected;
		for (;;)
		{
			connected.value =
				CreateFileW(wide.c_str(), GENERIC_READ | GENERIC_WRITE, 0, nullptr, OPEN_EXISTING,
							FILE_FLAG_OVERLAPPED | SECURITY_SQOS_PRESENT | SECURITY_IDENTIFICATION, nullptr);
			if (connected.value != INVALID_HANDLE_VALUE)
				break;
			const auto error = GetLastError();
			if (error != ERROR_PIPE_BUSY && error != ERROR_FILE_NOT_FOUND)
				throw native::Error("Open local control pipe failed", error);
			if (native::Clock::now() >= deadline)
				throw native::Error("Connect local control pipe timed out", error);
			if (error == ERROR_PIPE_BUSY)
				WaitNamedPipeW(wide.c_str(), 10);
			else
				Sleep(5);
		}
		native::VerifyPeer(connected.value, false, native::TokenSid(GetCurrentProcess()));
		pipe = std::make_unique<native::Channel>(std::move(connected));
	}
	std::unique_ptr<Exchange> Close() noexcept
	{
		SetEvent(stopping.value);
		if (pipe)
			pipe->Close();
		pipe.reset();
		return std::move(pending);
	}
	ControlPacket ExchangePacket()
	{
		const auto deadline = native::Clock::now() + timeout;
		try
		{
			while (pending->sent < pending->encoded.size())
				pending->sent += pipe->Transfer(true, std::span<std::byte>{pending->encoded}.subspan(pending->sent),
												deadline, stopping.value);
			for (;;)
			{
				while (pending->offset < pending->received.size())
					pending->offset +=
						pipe->Transfer(false, std::span<std::byte>{pending->received}.subspan(pending->offset),
									   deadline, stopping.value);
				auto decoded = Decode(pending->received);
				if (decoded.error == DecodeError::Incomplete && decoded.requiredBytes > pending->received.size())
				{
					pending->received.resize(decoded.requiredBytes);
					continue;
				}
				if (!decoded.message)
					throw std::runtime_error("Invalid control pipe response framing");
				auto response = std::move(*decoded.message);
				const auto count = response.Find(tag::HandleCount), error = response.Find(tag::Error);
				if (response.flags != 1 || response.opcode != pending->outgoing.opcode ||
					response.requestId != pending->outgoing.requestId || !error || !error->AsU32() ||
					(count && count->AsU32() != 0))
					throw std::runtime_error("Control pipe response identity/status/handle count mismatch");
				if (!*error->AsU32() && response.opcode == static_cast<std::uint16_t>(OpCode::ShareImage))
					throw std::runtime_error("Windows native image-handle transport is not implemented");
				return {std::move(response), {}, {}, {}};
			}
		}
		catch (const native::Timeout&)
		{
			throw ControlExchangeTimeout{pending->outgoing.requestId};
		}
		catch (const native::Interrupted&)
		{
			throw std::runtime_error("Control pipe connection closed");
		}
	}
};
LocalControlClient::LocalControlClient(std::string path, std::chrono::milliseconds timeout)
	: m_state(std::make_unique<State>(path, timeout))
{
}
LocalControlClient::~LocalControlClient()
{
	Close();
}
ControlPacket LocalControlClient::Request(ControlPacket request)
{
	std::unique_ptr<State::Exchange> retired;
	std::unique_lock lock(m_state->mutex);
	if (!m_state->pipe || WaitForSingleObject(m_state->stopping.value, 0) == WAIT_OBJECT_0)
		throw std::runtime_error("Control connection is closed");
	if (m_state->pending)
		throw std::logic_error("Control exchange unfinished; Retry or Close explicitly");
	if (request.message.flags || !request.handles.empty())
		throw std::invalid_argument("Windows control requests do not transfer native handles");
	if (const auto count = request.message.Find(tag::HandleCount); count && count->AsU32() != 0)
		throw std::invalid_argument("Windows control request handle count must be zero");
	auto exchange = std::make_unique<State::Exchange>();
	if (!request.message.requestId.value)
	{
		if (!m_state->next)
			throw std::overflow_error("Control request identity exhausted");
		request.message.requestId = {m_state->next};
	}
	exchange->outgoing = std::move(request.message);
	exchange->encoded = Encode(exchange->outgoing);
	exchange->received.resize(HeaderSize);
	if (m_state->next && exchange->outgoing.requestId.value >= m_state->next)
		m_state->next = exchange->outgoing.requestId.value == UINT64_MAX ? 0 : exchange->outgoing.requestId.value + 1;
	m_state->pending = std::move(exchange);
	try
	{
		auto response = m_state->ExchangePacket();
		retired = std::move(m_state->pending);
		return response;
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
		throw std::logic_error("No unfinished control pipe exchange");
	try
	{
		auto response = m_state->ExchangePacket();
		retired = std::move(m_state->pending);
		return response;
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
	return m_state->pending ? std::optional{m_state->pending->outgoing.requestId} : std::nullopt;
}
void LocalControlClient::Close() noexcept
{
	SetEvent(m_state->stopping.value);
	std::unique_ptr<State::Exchange> retired;
	std::lock_guard lock(m_state->mutex);
	retired = m_state->Close();
}
} // namespace simxr::control
