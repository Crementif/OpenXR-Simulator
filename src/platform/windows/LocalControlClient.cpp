#include "control/LocalControlClient.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "platform/windows/WindowsControl.h"

namespace simxr::control
{
namespace native = windows;

ControlExchangeTimeout::ControlExchangeTimeout(RequestId request)
	: std::runtime_error("Control exchange is unfinished; Retry or Close explicitly"), m_request(request)
{}

RequestId ControlExchangeTimeout::Request() const noexcept
{
	return m_request;
}

struct LocalControlClient::State
{
	struct Exchange
	{
		Message outgoing;
		std::shared_ptr<PacketExposure> exposure;
		std::vector<std::byte> encoded, received;
		std::size_t sent = 0, offset = 0;
		std::vector<std::byte> offered, incoming;
		std::array<std::byte, 8> acknowledged{};
		std::size_t offeredSent = 0, acknowledgedRead = 0, incomingRead = 0, acknowledgedSent = 0;
		bool imported = false;
		std::vector<std::shared_ptr<TransferHandle>> handles;
	};

	mutable std::mutex mutex;
	native::Handle stopping = native::Event();
	std::unique_ptr<native::Channel> pipe;
	std::unique_ptr<Exchange> pending;
	std::chrono::milliseconds timeout;
	std::uint64_t next = 1, peerProcess = 0;

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
				throw ControlConnectTimeout(native::Error("Connect local control pipe timed out", error).what());
			if (error == ERROR_PIPE_BUSY)
				WaitNamedPipeW(wide.c_str(), 10);
			else
				Sleep(5);
		}
		pipe = std::make_unique<native::Channel>(std::move(connected));
		pipe->Authenticate(false, native::TokenSid(GetCurrentProcess()));
		ULONG process = 0;
		if (!GetNamedPipeServerProcessId(pipe->Value(), &process) || !process)
			throw native::Error("Query control pipe peer process failed");
		peerProcess = process;
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
			if (!pending->offered.empty())
			{
				if (pending->offeredSent < pending->offered.size())
					pipe->PublishOffer(pending->exposure);
				while (pending->offeredSent < pending->offered.size())
					pending->offeredSent +=
						pipe->Transfer(true, std::span<std::byte>{pending->offered}.subspan(pending->offeredSent),
									   deadline, stopping.value);
				while (pending->acknowledgedRead < pending->acknowledged.size())
					pending->acknowledgedRead += pipe->Transfer(
						false, std::span<std::byte>{pending->acknowledged}.subspan(pending->acknowledgedRead), deadline,
						stopping.value);
				if (pending->acknowledged != native::TransferAcknowledgement(pending->outgoing.requestId))
					throw std::runtime_error("Windows request handle acknowledgement mismatch");
				pipe->AcknowledgeOffer();
			}
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
				const auto error = response.Find(tag::Error);
				if (response.flags != 1 || response.opcode != pending->outgoing.opcode ||
					response.requestId != pending->outgoing.requestId || !error || !error->AsU32())
					throw std::runtime_error("Control pipe response identity/status/handle count mismatch");
				const auto size = native::TransferBytes(response);
				const auto expected =
					!*error->AsU32() && response.opcode == static_cast<std::uint16_t>(OpCode::ShareImage) ? 16U : 0U;
				if (size != expected)
					throw std::runtime_error("Unexpected Windows response handles");
				if (size)
				{
					pending->incoming.resize(size);
					while (pending->incomingRead < size)
						pending->incomingRead += pipe->Transfer(
							false, std::span<std::byte>{pending->incoming}.subspan(pending->incomingRead), deadline,
							stopping.value);
					if (!pending->imported)
					{
						pending->handles = pipe->Import(pending->incoming);
						pending->imported = true;
					}
					auto acknowledgement = native::TransferAcknowledgement(response.requestId);
					while (pending->acknowledgedSent < acknowledgement.size())
						pending->acknowledgedSent += pipe->Transfer(
							true, std::span<std::byte>{acknowledgement}.subspan(pending->acknowledgedSent), deadline,
							stopping.value);
				}
				return {std::move(response), std::move(pending->handles), {}, {}};
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
{}

LocalControlClient::~LocalControlClient()
{
	Close();
}

std::uint64_t LocalControlClient::PeerProcess() const noexcept
{
	return m_state->peerProcess;
}

ControlPacket LocalControlClient::Request(ControlPacket request)
{
	std::unique_ptr<State::Exchange> retired;
	std::unique_ptr<State::Exchange> exchange;
	std::unique_lock lock(m_state->mutex);
	if (!m_state->pipe || WaitForSingleObject(m_state->stopping.value, 0) == WAIT_OBJECT_0)
		throw std::runtime_error("Control connection is closed");
	if (m_state->pending)
		throw std::logic_error("Control exchange unfinished; Retry or Close explicitly");
	if (request.message.flags)
		throw std::invalid_argument("Windows request contains response flags");
	if (!request.handles.empty() &&
		(request.handles.size() != 1 || request.message.opcode != static_cast<std::uint16_t>(OpCode::ShareImage)))
		throw std::invalid_argument("Only ShareImage transfers a consumer handle");
	exchange = std::make_unique<State::Exchange>();
	exchange->offered = m_state->pipe->Offer(request);
	exchange->exposure = std::move(request.exposure);
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
