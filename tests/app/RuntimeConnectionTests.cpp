#include "../TestSupport.h"
#include "app/services/RuntimeConnection.h"

#include <chrono>
#include <iostream>
#include <thread>

#if defined(__linux__)
#include "../control/FakeImageInspection.h"
#include "control/LocalControlServer.h"

#include <condition_variable>
#include <filesystem>
#include <functional>
#include <mutex>
#endif

namespace
{
using namespace simxr;
using namespace std::chrono_literals;
namespace app = simxr::app;
namespace wire = simxr::control;
wire::ControlPacket Packet(wire::OpCode opcode, std::vector<wire::Field> fields = {})
{
	wire::ControlPacket result;
	result.message.opcode = static_cast<std::uint16_t>(opcode);
	result.message.fields = std::move(fields);
	return result;
}
#if defined(__linux__)
struct Directory
{
	std::filesystem::path path;
	Directory()
	{
		char name[] = "/tmp/simxr-async-connection-XXXXXX";
		Require(::mkdtemp(name) != nullptr, "Create async endpoint directory");
		path = name;
	}
	~Directory()
	{
		std::filesystem::remove_all(path);
	}
};
struct Gate
{
	std::mutex mutex;
	std::condition_variable changed;
	bool entered = false, released = false;
	void Wait()
	{
		std::unique_lock lock(mutex);
		entered = true;
		changed.notify_all();
		changed.wait(lock, [&] { return released; });
	}
	void Await()
	{
		std::unique_lock lock(mutex);
		Require(changed.wait_for(lock, 3s, [&] { return entered; }), "Blocked provider was not entered");
	}
	void Release()
	{
		std::lock_guard lock(mutex);
		released = true;
		changed.notify_all();
	}
};
struct ReleaseGate
{
	std::shared_ptr<Gate> gate;
	~ReleaseGate()
	{
		gate->Release();
	}
};
class BlockingReader final : public wire::ImageReader
{
  public:
	BlockingReader(std::shared_ptr<wire::ImageReader> reader, std::shared_ptr<Gate> gate)
		: reader(std::move(reader)), gate(std::move(gate))
	{
	}
	wire::ImageDescription Description() const override
	{
		gate->Wait();
		return reader->Description();
	}
	wire::ImageCapabilities Capabilities() const noexcept override
	{
		return reader->Capabilities();
	}
	CompletionStatus PollCompletion() const noexcept override
	{
		return reader->PollCompletion();
	}
	std::shared_ptr<wire::ImageDelivery> Share(const wire::ImageDeviceIdentity& identity,
											   const std::shared_ptr<wire::TransferHandle>& consumer) const override
	{
		return reader->Share(identity, consumer);
	}
	std::shared_ptr<wire::ImageExport> Export(std::optional<std::uint32_t> layer) const override
	{
		return reader->Export(layer);
	}

  private:
	std::shared_ptr<wire::ImageReader> reader;
	std::shared_ptr<Gate> gate;
};
class BlockingProvider final : public wire::ImageInspectionProvider
{
  public:
	BlockingProvider(std::shared_ptr<wire::test::State> state, std::shared_ptr<Gate> gate)
		: provider(std::make_shared<wire::test::Provider>(std::move(state))), gate(std::move(gate))
	{
	}
	wire::ImageCapabilities Capabilities(XrSession session) const noexcept override
	{
		return provider->Capabilities(session);
	}
	std::shared_ptr<wire::ImageReader> Retain(std::shared_ptr<const FrameOutput> output) const override
	{
		return std::make_shared<BlockingReader>(provider->Retain(std::move(output)), gate);
	}

  private:
	std::shared_ptr<wire::ImageInspectionProvider> provider;
	std::shared_ptr<Gate> gate;
};
std::uint64_t Id(const wire::Message& message, std::uint16_t tag)
{
	const auto* field = message.Find(tag);
	Require(field && field->AsU64(), "Missing async response identity");
	return *field->AsU64();
}
void Connected(app::RuntimeConnection& connection, std::uint64_t epoch)
{
	Eventually(
		[&]
		{
			const auto state = connection.Snapshot();
			return state.epoch == epoch && state.state == app::RuntimeConnectionState::Connected;
		},
		"Asynchronous Hello did not connect");
	Require(connection.Snapshot().hello.message.Find(wire::tag::Error)->AsU32() == 0, "Hello reply was not preserved");
}
app::RuntimeReply Reply(app::RuntimeConnection& connection, std::uint64_t ticket)
{
	std::vector<app::RuntimeReply> replies;
	Eventually(
		[&]
		{
			replies = connection.Poll();
			return !replies.empty();
		},
		"Asynchronous request did not reply");
	Require(replies.size() == 1 && replies.front().ticket == ticket, "Reply ticket changed or unexpectedly batched");
	return std::move(replies.front());
}
struct Source
{
	std::uint64_t session = 0, generation = 0;
};
Source Discover(app::RuntimeConnection& connection)
{
	auto reply = Reply(connection, connection.Submit(Packet(wire::OpCode::Sessions)));
	auto list = wire::DecodeFields(reply.packet.message.Find(wire::tag::Sessions)->value);
	Require(list.fields.size() == 1, "Expected one actual CPU fixture session");
	auto record = wire::DecodeFields(list.fields.front().value);
	wire::Message fields;
	fields.fields = std::move(record.fields);
	return {Id(fields, wire::tag::Session), Id(fields, wire::tag::Generation)};
}
wire::ControlPacket Retain(Source source, CaptureId capture)
{
	return Packet(wire::OpCode::RetainImage, {wire::Field::U64(wire::tag::Session, source.session),
											  wire::Field::U64(wire::tag::Generation, source.generation),
											  wire::Field::U64(wire::tag::CaptureId, capture.value)});
}
class StopExposure final : public wire::PacketExposure
{
  public:
	explicit StopExposure(app::RuntimeConnection& connection) : connection(connection) {}
	void MarkTransferred() noexcept override
	{
		connection.Close();
		stopped.store(true);
	}
	std::atomic<bool> stopped{false};

  private:
	app::RuntimeConnection& connection;
};
void ConnectionAndHandles()
{
	Directory directory;
	wire::test::Fixture fixture;
	wire::LocalControlServer server{fixture.service, (directory.path / "control.sock").string()};
	app::RuntimeConnection connection;
	Require(connection.Submit(Packet(wire::OpCode::Sessions)) == 0, "Disconnected connection admitted work");
	const auto epoch = connection.Connect(server.Path());
	Connected(connection, epoch);
	const auto source = Discover(connection);
	auto retained = Reply(connection, connection.Submit(Retain(source, fixture.capture)));
	Require(retained.error.empty() && retained.epoch == epoch, "Valid image reply became transport failure");
	const auto image = Id(retained.packet.message, wire::tag::ImageReaderId);
	const auto description = wire::test::Description();
	auto request =
		Packet(wire::OpCode::ShareImage, {wire::Field::U64(wire::tag::ImageReaderId, image),
										  wire::Field::Bytes(wire::tag::DeviceUuid, description.identity.device),
										  wire::Field::Bytes(wire::tag::DriverUuid, description.identity.driver)});
	request.handles.push_back(wire::OwnControlFd(::open("/dev/null", O_RDONLY | O_CLOEXEC)));
	auto shared = Reply(connection, connection.Submit(std::move(request)));
	Require(shared.error.empty() && shared.packet.handles.size() == 2 && fixture.state->exposures == 1,
			"Worker did not transfer descriptor ownership or mark exposure");
	auto unsupported = Packet(wire::OpCode::Status);
	unsupported.message.opcode = 65000;
	auto remoteError = Reply(connection, connection.Submit(std::move(unsupported)));
	Require(remoteError.error.empty() &&
				remoteError.packet.message.Find(wire::tag::Error)->AsU32() ==
					static_cast<std::uint32_t>(wire::ErrorCode::UnsupportedCommand) &&
				connection.Snapshot().state == app::RuntimeConnectionState::Connected,
			"Structured remote error was converted to success or transport failure");
	auto stop = std::make_shared<StopExposure>(connection);
	auto last =
		Packet(wire::OpCode::ShareImage, {wire::Field::U64(wire::tag::ImageReaderId, image),
										  wire::Field::Bytes(wire::tag::DeviceUuid, description.identity.device),
										  wire::Field::Bytes(wire::tag::DriverUuid, description.identity.driver)});
	last.handles.push_back(wire::OwnControlFd(::open("/dev/null", O_RDONLY | O_CLOEXEC)));
	last.exposure = stop;
	Require(connection.Submit(std::move(last)) != 0, "Submit worker callback stop fixture");
	Eventually([&] { return stop->stopped.load(); }, "Worker-origin stop deadlocked on self join");
	connection.Close(); // External owner still joins; no worker is detached.
	std::array<std::byte, 96> bytes{};
	Require(::pread(static_cast<int>(shared.packet.handles[0]->Value()), bytes.data(), bytes.size(), 0) == 96 &&
				std::equal(bytes.begin(), bytes.end(), wire::test::Pixels().begin()),
			"Reply FD did not outlive connection worker");
	Require(connection.Connect(server.Path()) == 0 && connection.Submit(Packet(wire::OpCode::Sessions)) == 0,
			"Closed asynchronous connection reopened admission");
}
void BoundedQueueAndExactRetry()
{
	Directory directory;
	wire::test::Fixture fixture;
	auto gate = std::make_shared<Gate>();
	auto provider = std::make_shared<BlockingProvider>(fixture.state, gate);
	wire::ControlService service{fixture.runtime, 16, provider};
	wire::LocalControlServer server{service, (directory.path / "control.sock").string()};
	ReleaseGate release{gate};
	app::RuntimeConnectionConfig config;
	config.maxPending = 3;
	config.maxReplies = 1;
	config.retrySlice = 10ms;
	app::RuntimeConnection connection{config};
	Connected(connection, connection.Connect(server.Path()));
	const auto source = Discover(connection);
	const auto first = connection.Submit(Retain(source, fixture.capture));
	gate->Await();
	const auto second = connection.Submit(Packet(wire::OpCode::Sessions));
	const auto third = connection.Submit(Packet(wire::OpCode::Sessions));
	Require(first && second && third && connection.Submit(Packet(wire::OpCode::Sessions)) == 0,
			"Admission did not bound queued, active and unpolled requests together");
	const auto start = std::chrono::steady_clock::now();
	Require(connection.Poll().empty() && connection.Snapshot().inFlight, "Blocked request fabricated a reply");
	Require(std::chrono::steady_clock::now() - start < 100ms, "UI inspection blocked on socket work");
	std::this_thread::sleep_for(80ms); // Several Retry slices, one server dispatch.
	Require(fixture.state->retains == 1, "Timeout retry admitted another RPC");
	gate->Release();
	Eventually([&] { return connection.Snapshot().ready == 1; }, "Blocked response did not finish");
	std::this_thread::sleep_for(30ms);
	Require(connection.Snapshot().queued == 2 && !connection.Snapshot().inFlight &&
				connection.Submit(Packet(wire::OpCode::Sessions)) == 0,
			"Full reply queue allowed unbounded request/result growth");
	const auto retained = Reply(connection, first);
	Require(retained.packet.message.requestId == wire::RequestId{3} && fixture.state->retains == 1,
			"Retry changed the original exchange identity");
	Require(Reply(connection, second).packet.message.requestId == wire::RequestId{4} &&
				Reply(connection, third).packet.message.requestId == wire::RequestId{5},
			"Queued reply order changed");
}
void ReconnectRejectsStaleReplies()
{
	Directory directory;
	wire::test::Fixture fixture;
	auto gate = std::make_shared<Gate>();
	auto provider = std::make_shared<BlockingProvider>(fixture.state, gate);
	wire::ControlService service{fixture.runtime, 16, provider};
	wire::LocalControlServer server{service, (directory.path / "control.sock").string()};
	ReleaseGate release{gate};
	app::RuntimeConnectionConfig config;
	config.retrySlice = 10ms;
	app::RuntimeConnection connection{config};
	const auto oldEpoch = connection.Connect(server.Path());
	Connected(connection, oldEpoch);
	const auto source = Discover(connection);
	Require(connection.Submit(Retain(source, fixture.capture)) != 0, "Submit old request");
	gate->Await();
	Require(connection.Submit(Packet(wire::OpCode::Sessions)) != 0, "Queue old request");
	const auto disconnectStart = std::chrono::steady_clock::now();
	connection.Disconnect();
	Require(connection.Snapshot().state == app::RuntimeConnectionState::Disconnected &&
				std::chrono::steady_clock::now() - disconnectStart < 100ms,
			"Disconnect waited on blocked server");
	const auto newEpoch = connection.Connect(server.Path());
	Require(newEpoch > oldEpoch, "Reconnect reused connection epoch");
	Connected(connection, newEpoch);
	gate->Release();
	std::this_thread::sleep_for(30ms);
	Require(connection.Poll().empty(), "Old connection request/reply leaked through reconnect");
	auto response = Reply(connection, connection.Submit(Packet(wire::OpCode::Sessions)));
	Require(response.epoch == newEpoch && response.packet.message.requestId == wire::RequestId{2},
			"New epoch reused old protocol IDs");
}
class ReentrantHandle final : public wire::TransferHandle
{
  public:
	explicit ReentrantHandle(std::function<void()> release) : release(std::move(release)) {}
	~ReentrantHandle() override
	{
		release();
	}
	wire::TransferHandleKind Kind() const noexcept override
	{
		return wire::TransferHandleKind::OpaqueFd;
	}
	std::intptr_t Value() const noexcept override
	{
		return -1;
	}
	std::shared_ptr<wire::TransferHandle> Duplicate() const override
	{
		throw std::logic_error("Queued handle must never be sent");
	}

  private:
	std::function<void()> release;
};
void ResponsiveShutdownAndDeadline()
{
	for (bool deadline : {false, true})
	{
		Directory directory;
		wire::test::Fixture fixture;
		auto gate = std::make_shared<Gate>();
		auto provider = std::make_shared<BlockingProvider>(fixture.state, gate);
		wire::ControlService service{fixture.runtime, 16, provider};
		wire::LocalControlServer server{service, (directory.path / "control.sock").string()};
		ReleaseGate release{gate};
		app::RuntimeConnectionConfig config;
		config.retrySlice = 10ms;
		config.exchangeDeadline = deadline ? 60ms : 5000ms;
		app::RuntimeConnection connection{config};
		Connected(connection, connection.Connect(server.Path()));
		const auto source = Discover(connection);
		const auto ticket = connection.Submit(Retain(source, fixture.capture));
		gate->Await();
		if (deadline)
		{
			const auto queued = connection.Submit(Packet(wire::OpCode::Sessions));
			Eventually([&] { return connection.Snapshot().ready == 2; },
					   "Overall exchange deadline did not terminate bounded work");
			auto replies = connection.Poll();
			Require(replies.size() == 2 && replies[0].ticket == ticket && replies[0].timedOut &&
						!replies[0].error.empty() && replies[1].ticket == queued && !replies[1].error.empty() &&
						fixture.state->retains == 1 &&
						connection.Snapshot().state == app::RuntimeConnectionState::Failed &&
						connection.Snapshot().hello.message.Find(wire::tag::Error)->AsU32() == 0,
					"Deadline discarded diagnostic/Hello or silently resubmitted queued work");
			Require(replies[0].error == "Control request 3 (opcode " +
										  std::to_string(static_cast<std::uint16_t>(wire::OpCode::RetainImage)) +
										  ") exceeded the 60 ms exchange deadline; connection closed" &&
						connection.Snapshot().detail == replies[0].error,
					"Deadline omitted the stalled request identity, opcode or overall deadline");
		}
		else
		{
			bool reentered = false;
			auto request = Packet(wire::OpCode::Sessions);
			request.handles.push_back(std::make_shared<ReentrantHandle>(
				[&]
				{
					connection.Snapshot();
					connection.Close();
					reentered = true;
				}));
			Require(connection.Submit(std::move(request)) != 0, "Queue destructor reentry fixture");
			const auto start = std::chrono::steady_clock::now();
			connection.Close();
			Require(reentered && std::chrono::steady_clock::now() - start < 500ms,
					"Shutdown blocked or destroyed packet readers under lifecycle/state lock");
		}
		connection.Disconnect();
		connection.Close();
	}
}
void InvalidBounds()
{
	for (int invalid = 0; invalid < 6; ++invalid)
	{
		app::RuntimeConnectionConfig config;
		switch (invalid)
		{
		case 0:
			config.maxPending = 0;
			break;
		case 1:
			config.maxReplies = 65;
			break;
		case 2:
			config.retrySlice = 0ms;
			break;
		case 3:
			config.retrySlice = 101ms;
			break;
		case 4:
			config.exchangeDeadline = 1ms;
			break;
		case 5:
			config.exchangeDeadline = 31s;
			break;
		}
		bool rejected = false;
		try
		{
			app::RuntimeConnection connection{config};
		}
		catch (const std::invalid_argument&)
		{
			rejected = true;
		}
		Require(rejected, "Invalid asynchronous connection bound accepted");
	}
}
void ExplicitFailures()
{
	app::RuntimeConnection connection;
	const auto epoch = connection.Connect("/tmp/nonexistent-simxr-async-endpoint/socket");
	Eventually([&] { return connection.Snapshot().state == app::RuntimeConnectionState::Failed; },
			   "Missing endpoint did not report connection failure");
	Require(connection.Snapshot().epoch == epoch && !connection.Snapshot().detail.empty(),
			"Connection failure lost its epoch/detail");
	connection.Disconnect();
	Require(connection.Snapshot().state == app::RuntimeConnectionState::Disconnected && connection.Poll().empty(),
			"Disconnect did not clear failed connection");
}
#endif
} // namespace
int main()
{
	try
	{
#if defined(__linux__)
		ConnectionAndHandles();
		BoundedQueueAndExactRetry();
		ReconnectRejectsStaleReplies();
		ResponsiveShutdownAndDeadline();
		InvalidBounds();
		ExplicitFailures();
#else
		app::RuntimeConnection connection;
		connection.Connect("unsupported");
		Eventually([&] { return connection.Snapshot().state == app::RuntimeConnectionState::Failed; },
				   "Unsupported transport did not fail explicitly");
#endif
		std::cout << "PASS async runtime connection CPU transport, bounded ownership and epochs\n";
		return 0;
	}
	catch (const std::exception& error)
	{
		std::cerr << "FAIL: " << error.what() << '\n';
		return 1;
	}
}
