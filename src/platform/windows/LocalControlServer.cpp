#include "control/LocalControlServer.h"
#include "Pch.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "platform/windows/WindowsControl.h"


namespace simxr::control
{
namespace native = windows;
static void Read(native::Channel& pipe, std::vector<std::byte>& bytes, std::size_t total, HANDLE stop,
				 native::Clock::time_point& deadline)
{
	auto offset = bytes.size();
	bytes.resize(total);
	while (offset < total)
	{
		offset += pipe.Transfer(false, std::span<std::byte>{bytes}.subspan(offset), deadline, stop);
		if (deadline == native::Clock::time_point::max())
			deadline = native::Clock::now() + std::chrono::seconds(5);
	}
}
static void Write(native::Channel& pipe, const ControlPacket& packet, HANDLE stop)
{
	auto handles = pipe.Offer(packet);
	const auto fallback = packet.encoded ? std::vector<std::byte>{} : Encode(packet.message);
	const auto bytes =
		packet.encoded ? std::span<const std::byte>{*packet.encoded} : std::span<const std::byte>{fallback};
	const auto deadline = native::Clock::now() + std::chrono::seconds(5);
	std::size_t offset = 0;
	while (offset < bytes.size())
		offset +=
			pipe.Transfer(true, {const_cast<std::byte*>(bytes.data() + offset), bytes.size() - offset}, deadline, stop);
	if (!handles.empty())
	{
		pipe.PublishOffer(packet.exposure);
		offset = 0;
		while (offset < handles.size())
			offset += pipe.Transfer(true, std::span<std::byte>{handles}.subspan(offset), deadline, stop);
		std::vector<std::byte> acknowledgement;
		auto readDeadline = deadline;
		Read(pipe, acknowledgement, 8, stop, readDeadline);
		const auto expected = native::TransferAcknowledgement(packet.message.requestId);
		if (!std::equal(acknowledgement.begin(), acknowledgement.end(), expected.begin()))
			throw std::runtime_error("Windows handle acknowledgement identity mismatch");
		pipe.AcknowledgeOffer();
	}
}
static std::uint64_t ReadHeaderInteger(std::span<const std::byte> bytes, std::size_t offset, std::size_t count)
{
	std::uint64_t result = 0;
	for (std::size_t index = 0; index < count; ++index)
		result |= std::uint64_t{std::to_integer<unsigned char>(bytes[offset + index])} << (index * 8);
	return result;
}

struct LocalControlServer::State
{
	struct Worker
	{
		std::unique_ptr<native::Channel> pipe;
		std::atomic<bool> finished{false};
		std::thread thread;
	};
	ControlService& service;
	std::string path;
	std::wstring wide;
	std::size_t capacity;
	native::Security security;
	native::Handle endpoint;
	native::Handle stopping = native::Event();
	std::unique_ptr<native::Channel> listener;
	std::thread acceptor;
	std::vector<std::unique_ptr<Worker>> workers;
	std::mutex lifecycle;
	bool closed = false;
	State(ControlService& service, std::string path, std::size_t capacity)
		: service(service), path(std::move(path)), wide(native::PipePath(this->path)), capacity(capacity)
	{
		if (!capacity || capacity > 64)
			throw std::invalid_argument("Invalid control pipe client capacity");
		workers.reserve(capacity);
		// Keep an explicit simulator-owner reservation on Wine as well as Windows.
		auto suffix = wide.substr(std::wstring_view(L"\\\\.\\pipe\\").size());
		for (auto& character : suffix)
			if (character >= L'A' && character <= L'Z')
				character += L'a' - L'A';
		const auto name = L"Local\\" + suffix;
		SetLastError(ERROR_SUCCESS);
		endpoint = native::Handle(CreateMutexW(&security.attributes, FALSE, name.c_str()));
		const auto error = GetLastError();
		if (!endpoint.value || error == ERROR_ALREADY_EXISTS)
			throw native::Error("Local control endpoint already reserved or unavailable", error);
	}
	std::unique_ptr<native::Channel> Create(bool first)
	{
		native::Handle pipe(CreateNamedPipeW(
			wide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | (first ? FILE_FLAG_FIRST_PIPE_INSTANCE : 0),
			PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS,
			static_cast<DWORD>(capacity + 1), 65536, 65536, 5000, &security.attributes));
		if (pipe.value == INVALID_HANDLE_VALUE)
			throw native::Error("Create private local control pipe failed");
		return std::make_unique<native::Channel>(std::move(pipe));
	}
	void Serve(Worker& worker, ConnectionId connection) noexcept
	{
		try
		{
			while (WaitForSingleObject(stopping.value, 0) != WAIT_OBJECT_0)
			{
				std::vector<std::byte> bytes;
				auto deadline = native::Clock::time_point::max();
				Read(*worker.pipe, bytes, HeaderSize, stopping.value, deadline);
				auto decoded = Decode(bytes);
				if (decoded.error == DecodeError::UnsupportedVersion)
				{
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
					Write(*worker.pipe, {std::move(response), {}, {}, {}}, stopping.value);
					break;
				}
				if (decoded.error == DecodeError::Incomplete)
				{
					Read(*worker.pipe, bytes, decoded.requiredBytes, stopping.value, deadline);
					decoded = Decode(bytes);
				}
				if (!decoded.message || decoded.message->flags)
					break;
				ControlPacket request;
				request.message = std::move(*decoded.message);
				if (const auto size = native::TransferBytes(request.message))
				{
					std::vector<std::byte> handles;
					Read(*worker.pipe, handles, size, stopping.value, deadline);
					request.handles = worker.pipe->Import(handles);
					auto acknowledgement = native::TransferAcknowledgement(request.message.requestId);
					std::size_t offset = 0;
					while (offset < acknowledgement.size())
						offset += worker.pipe->Transfer(true, std::span<std::byte>{acknowledgement}.subspan(offset),
														deadline, stopping.value);
				}
				Write(*worker.pipe, service.HandlePacket(connection, std::move(request)), stopping.value);
			}
		}
		catch (const native::Interrupted&)
		{
		}
		catch (const native::Timeout&)
		{
			std::fputs("SimXR Windows control partial packet timed out\n", stderr);
		}
		catch (const std::exception& error)
		{
			std::fprintf(stderr, "SimXR Windows control connection: %s\n", error.what());
		}
		catch (...)
		{
			std::fputs("SimXR Windows control connection failed\n", stderr);
		}
		service.Disconnect(connection);
		worker.pipe->Close();
		worker.finished = true;
	}
	void Collect()
	{
		for (auto& worker : workers)
			if (worker->finished)
			{
				worker->thread.join();
				worker.reset();
			}
		std::erase(workers, nullptr);
	}
	void Accept() noexcept
	{
		while (WaitForSingleObject(stopping.value, 0) != WAIT_OBJECT_0)
		{
			try
			{
				Collect();
				if (workers.size() >= capacity)
				{
					WaitForSingleObject(stopping.value, 25);
					continue;
				}
				if (!listener)
					listener = Create(false);
				listener->Connect(native::Clock::time_point::max(), stopping.value);
				auto accepted = std::move(listener);
				// A live owned instance continuously reserves this pipe name.
				try
				{
					listener = Create(false);
				}
				catch (...)
				{
					DisconnectNamedPipe(accepted->Value());
					listener = std::move(accepted);
					throw;
				}
				accepted->Authenticate(true, security.sid);
				auto connection = service.Connect(accepted->NativeHandles());
				if (!connection)
					continue;
				try
				{
					auto worker = std::make_unique<Worker>();
					worker->pipe = std::move(accepted);
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
				}
				catch (...)
				{
					service.Disconnect(*connection);
					throw;
				}
			}
			catch (const native::Interrupted&)
			{
				break;
			}
			catch (const std::exception& error)
			{
				std::fprintf(stderr, "SimXR Windows control accept: %s\n", error.what());
				WaitForSingleObject(stopping.value, 50);
			}
			catch (...)
			{
				std::fputs("SimXR Windows control accept failed\n", stderr);
				WaitForSingleObject(stopping.value, 50);
			}
		}
		if (listener)
			listener->Close();
	}
};
LocalControlServer::LocalControlServer(ControlService& service, std::string path, std::size_t maxClients)
	: m_state(std::make_unique<State>(service, std::move(path), maxClients))
{
	m_state->listener = m_state->Create(true);
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
	if (m_state->closed)
		return;
	m_state->closed = true;
	SetEvent(m_state->stopping.value);
	if (m_state->acceptor.joinable())
		m_state->acceptor.join();
	for (auto& worker : m_state->workers)
		if (worker->thread.joinable())
			worker->thread.join();
	m_state->workers.clear();
	m_state->listener.reset();
	m_state->endpoint.Close();
}
} // namespace simxr::control
