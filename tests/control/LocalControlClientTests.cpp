#include "FakeImageInspection.h"
#include "control/LocalControlClient.h"
#include "control/LocalControlServer.h"

#include <array>
#include <chrono>
#include <filesystem>
#include <future>
#include <iostream>
#include <poll.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/un.h>
#include <thread>

namespace
{
using namespace simxr;
namespace wire = simxr::control;
using namespace std::chrono_literals;

struct Directory
{
	std::filesystem::path path;

	Directory()
	{
		std::array<char, 64> name{};
		std::strcpy(name.data(), "/tmp/simxr-native-client-XXXXXX");
		Require(::mkdtemp(name.data()) != nullptr, "Create native client test directory");
		path = name.data();
	}

	~Directory()
	{
		std::filesystem::remove_all(path);
	}
};

wire::ControlPacket Request(wire::OpCode op, std::vector<wire::Field> fields = {})
{
	wire::ControlPacket packet;
	packet.message.opcode = static_cast<std::uint16_t>(op);
	packet.message.fields = std::move(fields);
	return packet;
}

void TimeoutAndPartialRights()
{
	Directory directory;
	const auto path = (directory.path / "control.sock").string();
	auto listener = wire::OwnControlFd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::strcpy(address.sun_path, path.c_str());
	Require(::bind(static_cast<int>(listener->Value()), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
				::chmod(path.c_str(), 0600) == 0 && ::listen(static_cast<int>(listener->Value()), 1) == 0,
			"Bind native mock endpoint");
	std::promise<void> finish;
	auto gate = finish.get_future();
	std::exception_ptr failure;
	std::atomic<int> receivedCount{0};
	std::thread server([&] {
		try
		{
			auto peer =
				wire::OwnControlFd(::accept4(static_cast<int>(listener->Value()), nullptr, nullptr, SOCK_CLOEXEC));
			std::vector<std::byte> bytes(wire::HeaderSize);
			std::size_t offset = 0;
			std::vector<std::shared_ptr<wire::TransferHandle>> incoming;
			for (;;)
			{
				alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int) * 4)> ancillary{};
				iovec data{bytes.data() + offset, bytes.size() - offset};
				msghdr message{};
				message.msg_iov = &data;
				message.msg_iovlen = 1;
				message.msg_control = ancillary.data();
				message.msg_controllen = ancillary.size();
				const auto count = ::recvmsg(static_cast<int>(peer->Value()), &message, MSG_CMSG_CLOEXEC);
				Require(count > 0, "Receive native client request");
				for (auto* item = CMSG_FIRSTHDR(&message); item; item = CMSG_NXTHDR(&message, item))
				{
					const auto count = (item->cmsg_len - CMSG_LEN(0)) / sizeof(int);
					for (std::size_t index = 0; index < count; ++index)
					{
						int fd;
						std::memcpy(&fd, CMSG_DATA(item) + index * sizeof(int), sizeof(fd));
						incoming.push_back(wire::OwnControlFd(fd));
						++receivedCount;
					}
				}
				offset += count;
				if (offset != bytes.size())
					continue;
				auto decoded = wire::Decode(bytes);
				if (decoded.error == wire::DecodeError::Incomplete)
				{
					bytes.resize(decoded.requiredBytes);
					continue;
				}
				Require(decoded.message.has_value() && incoming.size() == 1,
						"Native client duplicated or omitted request FD");
				auto response = *decoded.message;
				response.flags = 1;
				response.fields = {wire::Field::U32(wire::tag::Error, 0), wire::Field::U32(wire::tag::HandleCount, 2)};
				auto encoded = wire::Encode(response);
				auto sendFd = [&](std::span<const std::byte> part) {
					alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int))> control{};
					iovec data{const_cast<std::byte*>(part.data()), part.size()};
					msghdr message{};
					message.msg_iov = &data;
					message.msg_iovlen = 1;
					message.msg_control = control.data();
					message.msg_controllen = control.size();
					auto* item = CMSG_FIRSTHDR(&message);
					item->cmsg_level = SOL_SOCKET;
					item->cmsg_type = SCM_RIGHTS;
					item->cmsg_len = CMSG_LEN(sizeof(int));
					const auto fd = static_cast<int>(incoming.front()->Value());
					std::memcpy(CMSG_DATA(item), &fd, sizeof(fd));
					Require(::sendmsg(static_cast<int>(peer->Value()), &message, MSG_NOSIGNAL) ==
								static_cast<ssize_t>(part.size()),
							"Send fragmented fixture FD");
				};
				sendFd(std::span<const std::byte>{encoded}.first(12));
				Require(gate.wait_for(3s) == std::future_status::ready, "Native client retry gate timed out");
				Require(::send(static_cast<int>(peer->Value()), encoded.data() + 12, wire::HeaderSize - 12,
							   MSG_NOSIGNAL) == static_cast<ssize_t>(wire::HeaderSize - 12),
						"Send remaining header");
				sendFd(std::span<const std::byte>{encoded}.subspan(wire::HeaderSize, 1));
				Require(::send(static_cast<int>(peer->Value()), encoded.data() + wire::HeaderSize + 1,
							   encoded.size() - wire::HeaderSize - 1,
							   MSG_NOSIGNAL) == static_cast<ssize_t>(encoded.size() - wire::HeaderSize - 1),
						"Send remaining payload");
				return;
			}
		}
		catch (...)
		{
			failure = std::current_exception();
		}
	});
	try
	{
		wire::LocalControlClient client{path, 20ms};
		auto fd = wire::OwnControlFd(::open("/dev/null", O_RDONLY | O_CLOEXEC));
		auto request = Request(wire::OpCode::ShareImage);
		request.handles.push_back(fd);
		bool timedOut = false;
		try
		{
			client.Request(request);
		}
		catch (const wire::ControlExchangeTimeout& error)
		{
			timedOut = error.Request() == wire::RequestId{1};
		}
		Require(timedOut && client.Pending() == wire::RequestId{1} &&
					::fcntl(static_cast<int>(fd->Value()), F_GETFD) >= 0,
				"Timeout did not preserve same request and caller descriptor");
		bool rejected = false;
		try
		{
			client.Request(Request(wire::OpCode::Sessions));
		}
		catch (const std::logic_error&)
		{
			rejected = true;
		}
		Require(rejected, "Pending exchange admitted another request");
		finish.set_value();
		auto result = client.Retry();
		Require(!client.Pending() && result.message.requestId == wire::RequestId{1} && result.handles.size() == 2 &&
					receivedCount == 1,
				"Retry changed identity or lost accumulated descriptors");
		for (const auto& handle : result.handles)
			Require(::fcntl(static_cast<int>(handle->Value()), F_GETFD) & FD_CLOEXEC,
					"Received native descriptor is inheritable");
		client.Close();
		Require(::fcntl(static_cast<int>(result.handles.front()->Value()), F_GETFD) >= 0,
				"Client close destroyed returned descriptor readers");
	}
	catch (...)
	{
		try
		{
			finish.set_value();
		}
		catch (...)
		{}
		server.join();
		throw;
	}
	server.join();
	if (failure)
		std::rethrow_exception(failure);
}

void MalformedResponseClosesRights()
{
	for (int malformed = 0; malformed < 3; ++malformed)
	{
		Directory directory;
		const auto path = (directory.path / "control.sock").string();
		auto listener = wire::OwnControlFd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
		sockaddr_un address{};
		address.sun_family = AF_UNIX;
		std::strcpy(address.sun_path, path.c_str());
		Require(::bind(static_cast<int>(listener->Value()), reinterpret_cast<sockaddr*>(&address), sizeof(address)) ==
						0 &&
					::chmod(path.c_str(), 0600) == 0 && ::listen(static_cast<int>(listener->Value()), 1) == 0,
				"Bind malformed response fixture");
		auto memory = wire::OwnControlFd(::memfd_create("simxr-malformed-response-fixture", MFD_CLOEXEC));
		struct stat identity{};
		Require(::fstat(static_cast<int>(memory->Value()), &identity) == 0, "Identify fixture descriptor");
		auto readers = [&] {
			int count = 0;
			for (int fd = 0; fd < 4096; ++fd)
			{
				struct stat current{};
				if (::fstat(fd, &current) == 0 && current.st_dev == identity.st_dev &&
					current.st_ino == identity.st_ino)
					++count;
			}
			return count;
		};
		std::exception_ptr failure;
		std::thread server([&] {
			try
			{
				auto peer =
					wire::OwnControlFd(::accept4(static_cast<int>(listener->Value()), nullptr, nullptr, SOCK_CLOEXEC));
				std::array<std::byte, wire::HeaderSize> header{};
				Require(::recv(static_cast<int>(peer->Value()), header.data(), header.size(), MSG_WAITALL) ==
							static_cast<ssize_t>(header.size()),
						"Read malformed fixture request");
				auto decoded = wire::Decode(header);
				Require(decoded.message.has_value(), "Decode malformed fixture request");
				auto response = *decoded.message;
				response.flags = 1;
				response.fields = {wire::Field::U32(wire::tag::Error, malformed == 2 ? 1 : 0),
								   wire::Field::U32(wire::tag::HandleCount, malformed == 0 ? 2 : 1)};
				auto encoded = wire::Encode(response);
				alignas(cmsghdr) std::array<std::byte, CMSG_SPACE(sizeof(int))> ancillary{};
				iovec data{encoded.data(), encoded.size()};
				msghdr message{};
				message.msg_iov = &data;
				message.msg_iovlen = 1;
				message.msg_control = ancillary.data();
				message.msg_controllen = ancillary.size();
				auto* rights = CMSG_FIRSTHDR(&message);
				rights->cmsg_level = SOL_SOCKET;
				rights->cmsg_type = SCM_RIGHTS;
				rights->cmsg_len = CMSG_LEN(sizeof(int));
				const int fd = static_cast<int>(memory->Value());
				std::memcpy(CMSG_DATA(rights), &fd, sizeof(fd));
				Require(::sendmsg(static_cast<int>(peer->Value()), &message, MSG_NOSIGNAL) ==
							static_cast<ssize_t>(encoded.size()),
						"Send malformed response with rights");
			}
			catch (...)
			{
				failure = std::current_exception();
			}
		});
		try
		{
			wire::LocalControlClient client{path};
			bool rejected = false;
			try
			{
				client.Request(Request(wire::OpCode::Hello));
			}
			catch (const std::runtime_error&)
			{
				rejected = true;
			}
			Require(rejected && !client.Pending(), "Malformed native response retained an exchange");
			client.Close();
		}
		catch (...)
		{
			server.join();
			throw;
		}
		server.join();
		if (failure)
			std::rethrow_exception(failure);
		Require(readers() == 1, "Malformed native response leaked installed descriptors");
	}
}

void InterruptClientClose()
{
	Directory directory;
	const auto path = (directory.path / "control.sock").string();
	auto listener = wire::OwnControlFd(::socket(AF_UNIX, SOCK_STREAM | SOCK_CLOEXEC, 0));
	sockaddr_un address{};
	address.sun_family = AF_UNIX;
	std::strcpy(address.sun_path, path.c_str());
	Require(::bind(static_cast<int>(listener->Value()), reinterpret_cast<sockaddr*>(&address), sizeof(address)) == 0 &&
				::chmod(path.c_str(), 0600) == 0 && ::listen(static_cast<int>(listener->Value()), 1) == 0,
			"Bind interruptible client fixture");
	wire::LocalControlClient client{path, 30s};
	auto peer = wire::OwnControlFd(::accept4(static_cast<int>(listener->Value()), nullptr, nullptr, SOCK_CLOEXEC));
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
	pollfd admitted{static_cast<int>(peer->Value()), POLLIN, 0};
	Require(::poll(&admitted, 1, 3000) == 1 && (admitted.revents & POLLIN), "Client admits the request before close");
	std::array<std::byte, wire::HeaderSize> header{};
	Require(::recv(static_cast<int>(peer->Value()), header.data(), header.size(), MSG_WAITALL) ==
				static_cast<ssize_t>(header.size()),
			"Read client request before withholding response");
	const auto start = std::chrono::steady_clock::now();
	client.Close();
	Require(std::chrono::steady_clock::now() - start < 1500ms && waiting.get() && !client.Pending(),
			"Close interrupts a long pending socket exchange and releases its state");
	bool rejected = false;
	try
	{
		client.Request(Request(wire::OpCode::Hello));
	}
	catch (const std::runtime_error&)
	{
		rejected = true;
	}
	Require(rejected, "Closed client cannot admit another exchange");
}

void ActualEndpoint()
{
	Directory directory;
	wire::test::Fixture fixture;
	wire::LocalControlServer server{fixture.service, (directory.path / "control.sock").string()};
	wire::LocalControlClient client{server.Path()};
	const auto hello = client.Request(Request(wire::OpCode::Hello));
	Require(hello.message.Find(wire::tag::Error)->AsU32() == 0 && hello.message.requestId == wire::RequestId{1},
			"Native client explicit Hello");
	const auto sessions = client.Request(Request(wire::OpCode::Sessions));
	Require(sessions.message.Find(wire::tag::Error)->AsU32() == 0 && sessions.message.requestId == wire::RequestId{2},
			"Native client request identities");
	client.Close();
	server.Close();
}
} // namespace

int main()
{
	try
	{
		TimeoutAndPartialRights();
		MalformedResponseClosesRights();
		InterruptClientClose();
		ActualEndpoint();
		std::cout << "PASS native local client retry, ancillary ownership and real endpoint\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
