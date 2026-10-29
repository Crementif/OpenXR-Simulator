#include "../TestSupport.h"
#include "control/ControlSchema.h"
#include "control/ControlService.h"
#include "control/LocalControlClient.h"
#include "control/LocalControlServer.h"
#include "platform/windows/WindowsControl.h"
#include "runtime/Runtime.h"

#include <future>
#include <iostream>

namespace
{
namespace wire = simxr::control;
namespace native = simxr::control::windows;
using namespace std::chrono_literals;

std::string Path()
{
	static std::atomic<unsigned> serial{0};
	return std::string(R"(\\.\pipe\SimXR.Control.)") + std::to_string(GetCurrentProcessId()) + ".test." +
		   std::to_string(++serial);
}

wire::ControlPacket Request(wire::OpCode code)
{
	wire::ControlPacket result;
	result.message.opcode = static_cast<std::uint16_t>(code);
	return result;
}

wire::ControlPacket Finish(wire::LocalControlClient& client, wire::ControlPacket packet)
{
	bool retry = false;
	const auto deadline = native::Clock::now() + 3s;
	for (;;)
	{
		try
		{
			return retry ? client.Retry() : client.Request(std::move(packet));
		}
		catch (const wire::ControlExchangeTimeout&)
		{
			Require(native::Clock::now() < deadline, "Windows exchange exceeded its targeted deadline");
			retry = true;
		}
	}
}

void ServiceLifecycle()
{
	simxr::ExtensionRegistry extensions({});
	simxr::Runtime runtime(extensions);
	wire::ControlService service(runtime);
	const auto path = Path();
	wire::LocalControlServer server(service, path, 2);
	bool conflict = false;
	try
	{
		wire::LocalControlServer duplicate(service, path, 2);
	}
	catch (const std::exception&)
	{
		conflict = true;
	}
	Require(conflict, "First instance prevents another server from claiming its endpoint");
	auto alias = path;
	alias.replace(alias.find(".test."), 6, ".TEST.");
	conflict = false;
	try
	{
		wire::LocalControlServer duplicate(service, alias, 2);
	}
	catch (const std::exception&)
	{
		conflict = true;
	}
	Require(conflict, "Endpoint reservation respects case-insensitive pipe names");
	wire::LocalControlClient client(path, 50ms);
	const auto hello = Finish(client, Request(wire::OpCode::Hello));
	Require(hello.message.Find(wire::tag::Error)->AsU32() == 0 && hello.handles.empty(),
			"Negotiate CPU-only Windows control");
	const auto capabilities = hello.message.Find(wire::tag::Capabilities)->AsU64().value();
	Require(!(capabilities & static_cast<std::uint64_t>(wire::Capability::GpuPreview)),
			"Windows transport does not invent GPU sharing");
	const auto sessions = Finish(client, Request(wire::OpCode::Sessions));
	Require(sessions.message.Find(wire::tag::Error)->AsU32() == 0, "Use the normal versioned request protocol");
	auto invalid = Request(wire::OpCode::Status);
	invalid.message.fields.push_back(wire::Field::U32(wire::tag::HandleCount, 1));
	bool rejected = false;
	try
	{
		client.Request(std::move(invalid));
	}
	catch (const std::invalid_argument&)
	{
		rejected = true;
	}
	Require(rejected, "Unsupported native handles reject before request admission");
	Require(Finish(client, Request(wire::OpCode::Sessions)).message.Find(wire::tag::Error)->AsU32() == 0,
			"Rejected native handles preserve the connection");
	auto nativeRequest = Request(wire::OpCode::ShareImage);
	nativeRequest.message.fields.push_back(wire::Field::U32(wire::tag::HandleCount, 1));
	auto consumer = native::Event();
	nativeRequest.handles.push_back(native::DuplicateTransfer(GetCurrentProcess(), consumer.value));
	Require(Finish(client, std::move(nativeRequest)).message.Find(wire::tag::Error)->AsU32() != 0,
			"Production server acknowledges native transfer before rejecting incomplete image request");
	const auto start = native::Clock::now();
	server.Close();
	Require(native::Clock::now() - start < 1500ms, "Server shutdown interrupts idle reads and joins its workers");
	client.Close();
	server.Close();
	wire::LocalControlServer restarted(service, path, 2);
	wire::LocalControlClient next(path, 50ms);
	Require(Finish(next, Request(wire::OpCode::Hello)).message.Find(wire::tag::Error)->AsU32() == 0,
			"Endpoint can restart after actual native closure");
}

struct Mock
{
	std::string path = Path();
	native::Security security;
	native::Handle stop = native::Event();
	std::unique_ptr<native::Channel> channel;
	std::thread thread;
	std::exception_ptr failure;
	std::atomic<bool> received{false};

	Mock()
	{
		const auto wide = native::PipePath(path);
		native::Handle handle(
			CreateNamedPipeW(wide.c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
							 PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_WAIT | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536,
							 65536, 5000, &security.attributes));
		Require(handle.value != INVALID_HANDLE_VALUE, "Create private native mock pipe");
		channel = std::make_unique<native::Channel>(std::move(handle));
	}

	~Mock()
	{
		SetEvent(stop.value);
		if (thread.joinable())
			thread.join();
	}

	wire::Message Read()
	{
		channel->Connect(native::Clock::time_point::max(), stop.value);
		std::vector<std::byte> bytes(wire::HeaderSize);
		std::size_t offset = 0;
		for (;;)
		{
			while (offset < bytes.size())
				offset += channel->Transfer(false, std::span<std::byte>{bytes}.subspan(offset),
											native::Clock::now() + 3s, stop.value);
			auto decoded = wire::Decode(bytes);
			if (decoded.error == wire::DecodeError::Incomplete)
			{
				bytes.resize(decoded.requiredBytes);
				continue;
			}
			Require(decoded.message.has_value(), "Decode mock request");
			received = true;
			return std::move(*decoded.message);
		}
	}
};

void Transfer(native::Channel& channel, bool write, std::span<std::byte> bytes)
{
	std::size_t offset = 0;
	const auto deadline = native::Clock::now() + 5s;
	while (offset < bytes.size())
		offset += channel.Transfer(write, bytes.subspan(offset), deadline, nullptr);
}

int HandleChild(const std::string& path, bool abandon)
{
	native::Security security;
	native::Handle pipe(CreateNamedPipeW(
		native::PipePath(path).c_str(), PIPE_ACCESS_DUPLEX | FILE_FLAG_OVERLAPPED | FILE_FLAG_FIRST_PIPE_INSTANCE,
		PIPE_TYPE_BYTE | PIPE_READMODE_BYTE | PIPE_REJECT_REMOTE_CLIENTS, 1, 65536, 65536, 5000, &security.attributes));
	Require(pipe.value != INVALID_HANDLE_VALUE, "Create child handle pipe");
	native::Channel channel(std::move(pipe));
	channel.Connect(native::Clock::now() + 5s, nullptr);
	channel.Authenticate(true, security.sid);
	std::vector<std::byte> bytes(wire::HeaderSize);
	Transfer(channel, false, bytes);
	auto decoded = wire::Decode(bytes);
	Require(decoded.error == wire::DecodeError::Incomplete, "Decode native request header");
	const auto header = bytes.size();
	bytes.resize(decoded.requiredBytes);
	Transfer(channel, false, std::span<std::byte>{bytes}.subspan(header));
	decoded = wire::Decode(bytes);
	Require(decoded.message.has_value(), "Decode native handle request");
	const auto& request = *decoded.message;
	std::vector<std::byte> incoming(native::TransferBytes(request));
	Transfer(channel, false, incoming);
	if (abandon)
		Sleep(100);
	auto handles = channel.Import(incoming);
	Require(handles.size() == 1, "Import consumer event from another process");
	Require(SetEvent(reinterpret_cast<HANDLE>(handles[0]->Value())), "Signal imported consumer event");
	if (abandon)
		return 0;
	Sleep(75); // Force Retry while the sender still owns the advertised handle number.
	auto acknowledgement = native::TransferAcknowledgement(request.requestId);
	Transfer(channel, true, acknowledgement);
	native::Handle memory(CreateFileMappingW(INVALID_HANDLE_VALUE, nullptr, PAGE_READWRITE, 0, 4096, nullptr));
	Require(memory.value != nullptr, "Create private mapped payload");
	auto view = static_cast<unsigned*>(MapViewOfFile(memory.value, FILE_MAP_WRITE, 0, 0, 4096));
	Require(view != nullptr, "Map payload");
	*view = 0x1234abcd;
	UnmapViewOfFile(view);
	wire::ControlPacket response;
	response.message.flags = 1;
	response.message.opcode = request.opcode;
	response.message.requestId = request.requestId;
	response.message.fields = {wire::Field::U32(wire::tag::Error, 0), wire::Field::U32(wire::tag::HandleCount, 2)};
	response.handles = {handles[0], native::DuplicateTransfer(GetCurrentProcess(), memory.value)};
	auto offered = channel.Offer(response);
	auto encoded = wire::Encode(response.message);
	Transfer(channel, true, encoded);
	channel.PublishOffer();
	Transfer(channel, true, std::span<std::byte>{offered}.first(3));
	Sleep(30);
	Transfer(channel, true, std::span<std::byte>{offered}.subspan(3));
	std::array<std::byte, 8> received{};
	Transfer(channel, false, received);
	Require(received == acknowledgement, "Acknowledge imported response handles");
	channel.AcknowledgeOffer();
	return 0;
}

struct TransferExposure final : wire::PacketExposure
{
	bool transferred = false;

	void MarkTransferred() noexcept override
	{
		transferred = true;
	}
};

void NativeHandlesAcrossProcesses(bool abandon = false)
{
	wchar_t executable[32768]{};
	Require(GetModuleFileNameW(nullptr, executable, 32768) != 0, "Locate native fixture executable");
	const auto path = Path();
	std::wstring command = L"\"" + std::wstring(executable) +
						   (abandon ? L"\" --abandoned-child " : L"\" --handle-child ") +
						   std::wstring(path.begin(), path.end());
	STARTUPINFOW startup{sizeof(startup)};
	PROCESS_INFORMATION process{};
	Require(
		CreateProcessW(executable, command.data(), nullptr, nullptr, FALSE, 0, nullptr, nullptr, &startup, &process),
		"Start distinct native handle peer without inherited handles");
	native::Handle child(process.hProcess), thread(process.hThread);
	try
	{
		const auto readyDeadline = native::Clock::now() + 5s;
		while (!WaitNamedPipeW(native::PipePath(path).c_str(), 10))
		{
			Require(native::Clock::now() < readyDeadline, "Child pipe became available");
			Sleep(5);
		}
		wire::LocalControlClient client(path, 20ms);
		auto event = native::Event();
		auto packet = Request(wire::OpCode::ShareImage);
		packet.message.fields.push_back(wire::Field::U32(wire::tag::HandleCount, 1));
		packet.handles.push_back(native::DuplicateTransfer(GetCurrentProcess(), event.value));
		auto exposure = std::make_shared<TransferExposure>();
		packet.exposure = exposure;
		if (abandon)
		{
			try
			{
				client.Request(std::move(packet));
			}
			catch (const wire::ControlExchangeTimeout&)
			{}
			Require(client.Pending().has_value(), "Unacknowledged request stays pending");
			Require(exposure->transferred, "Timed-out native handle offer reports possible peer exposure");
			client.Close();
			Require(WaitForSingleObject(event.value, 5000) == WAIT_OBJECT_0,
					"Closed sender preserves advertised handles until the delayed peer duplicates them");
			Require(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0, "Delayed peer exited");
			DWORD result = 1;
			Require(GetExitCodeProcess(child.value, &result) && result == 0, "Delayed peer imported safely");
			return;
		}
		auto response = Finish(client, std::move(packet));
		Require(exposure->transferred, "Acknowledged native handle offer reports peer exposure");
		Require(WaitForSingleObject(event.value, 0) == WAIT_OBJECT_0 && response.handles.size() == 2,
				"Other process used the incoming consumer handle and returned two owned handles");
		Require(WaitForSingleObject(child.value, 5000) == WAIT_OBJECT_0, "Handle peer completed acknowledgement");
		DWORD result = 1;
		Require(GetExitCodeProcess(child.value, &result) && result == 0, "Native handle peer passed");
		event.Close();
		Require(WaitForSingleObject(reinterpret_cast<HANDLE>(response.handles[0]->Value()), 0) == WAIT_OBJECT_0,
				"Imported event survives source handles and peer process");
		auto view = static_cast<const unsigned*>(
			MapViewOfFile(reinterpret_cast<HANDLE>(response.handles[1]->Value()), FILE_MAP_READ, 0, 0, 4096));
		Require(view && *view == 0x1234abcd, "Imported memory survives peer exit with unchanged bytes");
		UnmapViewOfFile(view);
		std::cout << "PASS cross-process Win32 event/memory transfer, partial bytes, retry and peer exit\n";
	}
	catch (...)
	{
		TerminateProcess(child.value, 1);
		WaitForSingleObject(child.value, 5000);
		throw;
	}
}

void RetryAndPartialBytes()
{
	Mock mock;
	mock.thread = std::thread([&] {
		try
		{
			auto request = mock.Read();
			WaitForSingleObject(mock.stop.value, 75);
			DWORD available = 0;
			Require(PeekNamedPipe(mock.channel->Value(), nullptr, 0, nullptr, &available, nullptr) && !available,
					"Timeout retries do not retransmit another request");
			wire::Message response;
			response.flags = 1;
			response.opcode = request.opcode;
			response.requestId = request.requestId;
			response.fields = {wire::Field::U32(wire::tag::Error, 0),
							   wire::Field::Text(wire::tag::Detail, std::string(170000, 'x'))};
			auto bytes = wire::Encode(response);
			std::size_t sent = mock.channel->Transfer(true, std::span<std::byte>{bytes}.first(3),
													  native::Clock::now() + 3s, mock.stop.value);
			WaitForSingleObject(mock.stop.value, 30);
			while (sent < bytes.size())
				sent += mock.channel->Transfer(true, std::span<std::byte>{bytes}.subspan(sent),
											   native::Clock::now() + 3s, mock.stop.value);
		}
		catch (...)
		{
			mock.failure = std::current_exception();
		}
	});
	wire::LocalControlClient client(mock.path, 10ms);
	bool timedOut = false;
	try
	{
		client.Request(Request(wire::OpCode::Hello));
	}
	catch (const wire::ControlExchangeTimeout& timeout)
	{
		timedOut = true;
		Require(client.Pending() == timeout.Request(), "Timeout preserves the exact request identity");
	}
	Require(timedOut, "Native pending response causes an explicit retryable timeout");
	const auto pending = client.Pending();
	bool rejected = false;
	try
	{
		client.Request(Request(wire::OpCode::Sessions));
	}
	catch (const std::logic_error&)
	{
		rejected = true;
	}
	Require(rejected && client.Pending() == pending, "New requests cannot replace a pending exchange");
	wire::ControlPacket response;
	const auto deadline = native::Clock::now() + 3s;
	for (;;)
	{
		try
		{
			response = client.Retry();
			break;
		}
		catch (const wire::ControlExchangeTimeout&)
		{
			Require(native::Clock::now() < deadline, "Native partial response exceeded its targeted deadline");
		}
	}
	Require(response.message.requestId == *pending &&
				response.message.Find(wire::tag::Detail)->AsText()->size() == 170000 && !client.Pending(),
			"Partial header and large payload resume exactly once");
	mock.thread.join();
	if (mock.failure)
		std::rethrow_exception(mock.failure);
}

void InterruptClientClose()
{
	Mock mock;
	mock.thread = std::thread([&] {
		try
		{
			mock.Read();
			WaitForSingleObject(mock.stop.value, 5000);
		}
		catch (...)
		{
			mock.failure = std::current_exception();
		}
	});
	wire::LocalControlClient client(mock.path, 30s);
	auto waiting = std::async(std::launch::async, [&] {
		try
		{
			client.Request(Request(wire::OpCode::Hello));
			return false;
		}
		catch (const std::exception&)
		{
			return true;
		}
	});
	Eventually([&] { return mock.received.load(); }, "Mock receives the admitted request");
	const auto start = native::Clock::now();
	client.Close();
	Require(native::Clock::now() - start < 1500ms && waiting.get(),
			"Close interrupts a long pending operation without freeing an active OVERLAPPED");
}
} // namespace

int main(int argc, char** argv)
{
	try
	{
		if (argc == 3)
			return HandleChild(argv[2], std::string_view(argv[1]) == "--abandoned-child");
		ServiceLifecycle();
		NativeHandlesAcrossProcesses();
		NativeHandlesAcrossProcesses(true);
		RetryAndPartialBytes();
		InterruptClientClose();
		std::cout << "PASS Windows same-user named-pipe control transport\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << error.what() << '\n';
		return 1;
	}
}
